#include "automation_files.h"
#include "automation_scene_host.h"
#include "automation_jobs.h"
#include "automation_shutdown.h"
#include "automation_shared_parse.h"

#include "toonz/toonzscene.h"
#include "toonz/tproject.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <map>

#ifdef _WIN32
#include <qt_windows.h>
#endif

namespace GroundzeroAutomation {

struct SceneHost final {
  QString documentId;
  QString directory;
  std::unique_ptr<QProcess> process;
  QByteArray buffer;
  QString lostCause;
  QString lostPhase;
  QString lastJob;
  bool renderActive = false;
#ifdef _WIN32
  // A job that ends the host when the worker's handle to it closes, including
  // when the worker itself is killed.
  HANDLE lifetime = nullptr;
#endif

  bool lost() const { return !lostCause.isEmpty(); }
};

struct ProjectEntry final {
  QString file;
  QString folder;
  QString name;
};

struct SceneHostRegistry final {
  std::map<QString, std::unique_ptr<SceneHost>> hosts;
  QHash<QString, ProjectEntry> projects;
  QHash<QString, QString> jobs;
  // The last record each host reported for its jobs.
  QHash<QString, QJsonObject> records;
  QHash<QString, QString> artifacts;
  quint64 requests = 0;

  ~SceneHostRegistry();
};

namespace {

constexpr int MaxScanDepth       = 4;
constexpr int MaxScanEntries     = 20000;
constexpr int MaxScanMs          = 10000;
constexpr int MaxProjects        = 4096;
constexpr qint64 MaxHashedBytes  = 64LL * 1024 * 1024;
constexpr int MaxRegistered      = 65536;
constexpr qint64 MaxHostLine     = 9LL * 1024 * 1024;
constexpr int HostMemoryExit     = 125;
constexpr int HostCloseGraceMs   = 2000;
constexpr int MaxAlternates      = 8;
constexpr int StandaloneNameSize = 256;

const QStringList& projectSuffixes() {
  static const QStringList suffixes = {"_otprj.xml", "_prj63ml.xml",
                                       "_prj6.xml", "_prj.xml"};
  return suffixes;
}

QString projectFormat(const QString& file) {
  const QString name = QFileInfo(file).fileName();
  if (name == "tahomaproject.xml") return "tahoma";
  for (const QString& suffix : projectSuffixes())
    if (name.endsWith(suffix)) return suffix.mid(1, suffix.size() - 5);
  return QString();
}

SceneHostRegistry& registry(Context& context) {
  if (!context.sceneHosts)
    context.sceneHosts = std::make_shared<SceneHostRegistry>();
  return *context.sceneHosts;
}

bool beneath(const QString& path, const QString& root) {
  return path == root ||
         path.startsWith(root.endsWith('/') ? root : root + '/');
}

QStringList readRoots(const Context& context) {
  QStringList roots{context.workspace()};
  roots.append(context.inputRoots());
  return roots;
}

// ---------------------------------------------------------------- project.list

struct ProjectRow final {
  QString file;
  QString folder;
  QString name;
  qint64 size = 0;
  QString revision;
  QString status;
  QStringList alternates;
};

struct Scan final {
  Context& context;
  int maxDepth;
  QElapsedTimer timer;
  int entries   = 0;
  qint64 links  = 0;
  qint64 hashed = 0;
  QStringList reasons;
  QSet<QString> visited;
  std::vector<ProjectRow> rows;

  void incomplete(const QString& reason) {
    if (!reasons.contains(reason)) reasons.append(reason);
  }
};

// The workspace's private runtime, render outputs and worker projects.
// Entries of one approved folder in name order, listed through a descriptor
// pinned to it; a folder that cannot be read lists nothing.
QVector<EntryInfo> listedEntries(const QString& folder, bool& readable,
                                 bool& truncated, int maximum) {
  QVector<EntryInfo> entries;
  const ListResult result = listApprovedDirectory(folder, entries, maximum);
  readable                = result != ListResult::Failed;
  truncated               = result == ListResult::TooMany;
  std::sort(entries.begin(), entries.end(),
            [](const EntryInfo& left, const EntryInfo& right) {
              return left.fileName() < right.fileName();
            });
  return entries;
}

bool skippedDirectory(const Context& context, const EntryInfo& entry) {
  if (QFileInfo(entry.absolutePath()).canonicalFilePath() !=
      context.workspace())
    return false;
  const QString name = entry.fileName();
  return name == "renders" || name.startsWith("runtime_") ||
         (name.startsWith("project_") &&
          QFileInfo(
              QDir(entry.absoluteFilePath()).filePath("automation_otprj.xml"))
              .isFile());
}

QStringList alternateFiles(const QVector<EntryInfo>& entries,
                           const QString& chosen) {
  QStringList result;
  for (const EntryInfo& entry : entries) {
    const QString name = entry.fileName();
    bool project       = name == "tahomaproject.xml";
    for (const QString& suffix : projectSuffixes())
      project = project || name.endsWith(suffix);
    if (!project || !entry.isFile() || entry.absoluteFilePath() == chosen)
      continue;
    if (result.size() < MaxAlternates) result.append(entry.absoluteFilePath());
  }
  return result;
}

QStringList alternateFiles(const QString& folder, const QString& chosen) {
  bool readable  = false;
  bool truncated = false;
  return alternateFiles(listedEntries(folder, readable, truncated, 0), chosen);
}

void hashProject(Scan& scan, ProjectRow& row) {
  if (row.size > ProjectFileLimitBytes) {
    row.status = "too_large";
    return;
  }
  if (scan.hashed + row.size > MaxHashedBytes) {
    row.status = "not_hashed";
    scan.incomplete("hash_budget");
    return;
  }
  QFile file;
  try {
    if (openApprovedFile(file, row.file) != OpenResult::Opened) {
      row.status = "unreadable";
      return;
    }
  } catch (const Error&) {
    row.status = "unreadable";
    scan.incomplete("skipped_links");
    return;
  }
  const QByteArray bytes = file.read(ProjectFileLimitBytes + 1);
  scan.hashed += bytes.size();
  row.revision = fileRevision(bytes);
  row.status   = "ok";
}

void detectProject(Scan& scan, const QString& folder,
                   const QVector<EntryInfo>& entries) {
  const TFilePath native =
      TProjectManager::instance()->projectFolderToProjectPath(
          TFilePath(folder.toStdWString()));
  const QString file = native.getQString();
  const QString name = QFileInfo(file).fileName();
  const auto found   = std::find_if(
      entries.begin(), entries.end(),
      [&](const EntryInfo& entry) { return entry.fileName() == name; });
  if (found == entries.end()) return;
  if (found->isSymLink()) {
    ++scan.links;
    scan.incomplete("skipped_links");
    return;
  }
  if (!found->isFile() || projectFormat(file).isEmpty()) return;
  if (int(scan.rows.size()) >= MaxProjects) {
    scan.incomplete("entry_limit");
    return;
  }
  ProjectRow row;
  row.file       = file;
  row.folder     = folder;
  row.name       = QFileInfo(folder).fileName();
  row.size       = found->size();
  row.alternates = alternateFiles(entries, file);
  if (row.name.isEmpty()) row.name = QFileInfo(file).completeBaseName();
  hashProject(scan, row);
  scan.rows.push_back(row);
}

bool scanBudget(Scan& scan) {
  if (scan.timer.elapsed() > MaxScanMs) {
    scan.incomplete("time_limit");
    return false;
  }
  if (scan.entries > MaxScanEntries) {
    scan.incomplete("entry_limit");
    return false;
  }
  return true;
}

void scanFolder(Scan& scan, const QString& folder, int depth) {
  if (!scanBudget(scan) || scan.visited.contains(folder)) return;
  scan.visited.insert(folder);
  bool readable  = false;
  bool truncated = false;
  const QVector<EntryInfo> entries =
      listedEntries(folder, readable, truncated, MaxScanEntries + 1);
  if (!readable) {
    scan.incomplete("unreadable");
    return;
  }
  if (truncated) scan.incomplete("entry_limit");
  detectProject(scan, folder, entries);
  for (const EntryInfo& entry : entries) {
    ++scan.entries;
    if (!scanBudget(scan)) return;
    if (entry.fileName().startsWith('.')) continue;
    if (entry.isSymLink()) {
      ++scan.links;
      scan.incomplete("skipped_links");
      continue;
    }
    if (!entry.isDir() || skippedDirectory(scan.context, entry)) continue;
    if (depth + 1 > scan.maxDepth) {
      scan.incomplete("depth_limit");
      continue;
    }
    scanFolder(scan, entry.absoluteFilePath(), depth + 1);
  }
}

QJsonObject projectRowJson(const ProjectRow& row, bool duplicate) {
  return {{"project_id", projectIdForFile(row.file)},
          {"name", row.name.left(StandaloneNameSize)},
          {"folder", row.folder},
          {"project_file", row.file},
          {"format", projectFormat(row.file)},
          {"size_bytes", double(row.size)},
          {"file_revision",
           row.revision.isEmpty() ? QJsonValue() : QJsonValue(row.revision)},
          {"status", row.status},
          {"alternates", QJsonArray::fromStringList(row.alternates)},
          {"duplicate_name", duplicate}};
}

QJsonArray projectRows(const std::vector<ProjectRow>& rows) {
  QHash<QString, int> names;
  for (const ProjectRow& row : rows) ++names[row.name];
  QJsonArray result;
  for (const ProjectRow& row : rows)
    result.append(projectRowJson(row, names.value(row.name) > 1));
  return result;
}

QStringList scanFolders(Context& context, const QJsonObject& params) {
  if (!params.contains("folder")) {
    QStringList folders;
    for (const QString& root : readRoots(context))
      if (QFileInfo(root).isDir()) folders.append(root);
    return folders;
  }
  const QString folder =
      context.resolvePath(requiredString(params, "folder"), false);
  const QFileInfo info(folder);
  if (!info.exists())
    throw Error("not_found", "The folder does not exist.", {{"path", folder}});
  if (!info.isDir())
    throw Error("invalid_path", "The folder is not a directory.",
                {{"path", folder}});
  return {folder};
}

QJsonArray rootRows(const Context& context) {
  QJsonArray roots;
  bool workspace = true;
  for (const QString& root : readRoots(context)) {
    roots.append(QJsonObject{{"path", root},
                             {"kind", workspace ? "workspace" : "input_root"},
                             {"available", QFileInfo(root).isDir()}});
    workspace = false;
  }
  return roots;
}

void registerProjects(Context& context, const QJsonArray& page) {
  auto& projects = registry(context).projects;
  for (const QJsonValue& value : page) {
    const QJsonObject row = value.toObject();
    const QString id      = row.value("project_id").toString();
    if (!projects.contains(id) && projects.size() >= MaxRegistered)
      throw Error("resource_limit",
                  "This worker has registered its project limit; start a new "
                  "worker.");
    projects.insert(
        id, {row.value("project_file").toString(),
             row.value("folder").toString(), row.value("name").toString()});
  }
}

QJsonObject standaloneDescription() {
  return {{"project_id", "standalone"},
          {"description",
           "No project: +alias paths use the native empty-project rule, "
           "$scenefolder resolves next to the scene and other project "
           "folders stay unresolved."}};
}

QJsonValue listProjects(Context& context, const QJsonObject& params) {
  const int depth  = integerParam(params, "depth", 1, MaxScanDepth, 4);
  const int offset = integerParam(params, "offset", 0, MaxProjects, 0);
  const int limit  = integerParam(params, "limit", 1, 64, 32);
  Scan scan{context, depth};
  scan.timer.start();
  const QStringList folders = scanFolders(context, params);
  for (const QString& folder : folders) scanFolder(scan, folder, 0);
  std::sort(
      scan.rows.begin(), scan.rows.end(),
      [](const ProjectRow& a, const ProjectRow& b) { return a.file < b.file; });
  const QJsonArray rows = projectRows(scan.rows);
  const QString revision =
      "listing:" +
      fileRevision(QJsonDocument(rows).toJson(QJsonDocument::Compact)).mid(7);
  if (params.contains("expected_listing_revision") &&
      params.value("expected_listing_revision").toString() != revision)
    throw Error("revision_conflict", "The project listing changed.",
                {{"scope", "listing"}, {"listing_revision", revision}});
  if (offset > rows.size())
    throw Error("invalid_params", "offset exceeds the discovered projects.");
  const int end = std::min(offset + limit, int(rows.size()));
  QJsonArray page;
  for (int index = offset; index < end; ++index) page.append(rows.at(index));
  registerProjects(context, page);
  return QJsonObject{
      {"roots", rootRows(context)},
      {"projects", page},
      {"standalone", standaloneDescription()},
      {"listing_revision", revision},
      {"offset", offset},
      {"limit", limit},
      {"discovered", int(rows.size())},
      {"next_offset", end < rows.size() ? QJsonValue(end) : QJsonValue()},
      {"scan",
       QJsonObject{{"complete", scan.reasons.isEmpty()},
                   {"reasons", QJsonArray::fromStringList(scan.reasons)},
                   {"entries_visited", scan.entries},
                   {"skipped_links", double(scan.links)}}}};
}

// ------------------------------------------------------------- host processes

[[noreturn]] void hostFailed(SceneHost& host, const QString& documentId) {
  throw Error("scene_host_failed",
              "The document's scene host ended; discard the document.",
              {{"document_id", documentId},
               {"cause", host.lostCause},
               {"phase", host.lostPhase}});
}

void stopProcess(SceneHost& host) {
  if (!host.process) return;
  if (host.process->state() != QProcess::NotRunning) {
    host.process->kill();
    host.process->waitForFinished(3000);
  }
}

// A lost host can publish nothing, so what it left on disk goes with it.
void markLost(SceneHost& host, const QString& cause, const QString& phase) {
  stopProcess(host);
  if (!host.directory.isEmpty()) QDir(host.directory).removeRecursively();
  host.lostCause = cause;
  host.lostPhase = phase;
}

QString exitCause(const QProcess& process) {
  if (process.exitStatus() == QProcess::NormalExit &&
      process.exitCode() == HostMemoryExit)
    return "memory";
  return "crashed";
}

QString breach(SceneHost& host, const QElapsedTimer& timer, int deadline) {
  QProcess& process = *host.process;
  if (process.state() == QProcess::NotRunning) return exitCause(process);
  if (host.buffer.size() > MaxHostLine) return "crashed";
  if (timer.elapsed() > deadline) return "timeout";
  if (processMemoryBytes(process.processId()) > SceneHostMemoryBytes)
    return "memory";
  return QString();
}

QJsonObject readReply(SceneHost& host, const QString& phase, int deadline) {
  QElapsedTimer timer;
  timer.start();
  while (!host.buffer.contains('\n')) {
    host.process->waitForReadyRead(20);
    host.buffer += host.process->readAllStandardOutput();
    if (host.buffer.contains('\n')) break;
    const QString cause = breach(host, timer, deadline);
    if (cause.isEmpty()) continue;
    markLost(host, cause, phase);
    hostFailed(host, host.documentId);
  }
  const int end         = host.buffer.indexOf('\n');
  const QByteArray line = host.buffer.left(end);
  host.buffer.remove(0, end + 1);
  QJsonParseError parse;
  const QJsonDocument reply = QJsonDocument::fromJson(line, &parse);
  if (parse.error != QJsonParseError::NoError || !reply.isObject()) {
    markLost(host, "crashed", phase);
    hostFailed(host, host.documentId);
  }
  return reply.object();
}

QJsonValue exchange(Context& context, SceneHost& host, const QString& method,
                    const QJsonObject& params, int deadline,
                    const QString& phase) {
  if (host.lost()) hostFailed(host, host.documentId);
  if (host.process->state() == QProcess::NotRunning) {
    markLost(host, exitCause(*host.process),
             host.renderActive ? "render" : phase);
    hostFailed(host, host.documentId);
  }
  const QString id = "host-" + QString::number(++registry(context).requests);
  QByteArray line =
      QJsonDocument(
          QJsonObject{{"id", id}, {"method", method}, {"params", params}})
          .toJson(QJsonDocument::Compact);
  line.append('\n');
  if (host.process->write(line) != line.size()) {
    markLost(host, "crashed", phase);
    hostFailed(host, host.documentId);
  }
  const QJsonObject reply = readReply(host, phase, deadline);
  if (reply.contains("error")) {
    const QJsonObject error = reply.value("error").toObject();
    throw Error(error.value("code").toString(),
                error.value("message").toString(),
                error.value("details").toObject());
  }
  return reply.value("result");
}

QStringList hostArguments(const Context& context, const QString& directory,
                          const QString& documentId) {
  const JobRegistry& jobs = *context.jobs;
  QStringList arguments{
      "--automation-worker", "--workspace",    directory,
      "--scene-host",        documentId,       "--worker-instance",
      jobs.instance(),       "--worker-epoch", QString::number(jobs.epochMs())};
  for (const QString& root : readRoots(context))
    arguments << "--input-root" << root;
  arguments << "--grants" << context.authority.grants.join(',') << "--domains"
            << context.authority.domains.join(',');
#ifdef BUILD_UI_PORT_TESTS
  if (!context.childFault.isEmpty())
    arguments << "--test-child-fault" << context.childFault;
  if (!context.pathSwap.isEmpty())
    arguments << "--test-path-swap" << context.pathSwap;
#endif
  return arguments;
}

// POSIX hosts watch their parent instead (startSceneHostWatchdog). On Windows
// the worker puts each host in a job of its own that kills it when the job's
// only handle, held by the worker, closes; nested jobs need Windows 8.
void bindHostLifetime(SceneHost& host) {
#ifdef _WIN32
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  const HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                     FALSE, DWORD(host.process->processId()));
  const bool bound =
      job && process && SetHandleInformation(job, HANDLE_FLAG_INHERIT, 0) &&
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits)) &&
      AssignProcessToJobObject(job, process);
  if (process) CloseHandle(process);
  if (bound) {
    host.lifetime = job;
    return;
  }
  if (job) CloseHandle(job);
  stopProcess(host);
  host.lostCause = "crashed";
#else
  Q_UNUSED(host);
#endif
}

std::unique_ptr<SceneHost> startHost(Context& context,
                                     const QString& documentId,
                                     const QByteArray& sceneBytes) {
  auto host        = std::make_unique<SceneHost>();
  host->documentId = documentId;
  QTemporaryDir directory(
      QDir(context.runtimeDirectory).filePath("scene-host-XXXXXX"));
  directory.setAutoRemove(false);
  host->directory = directory.path();
  QFile copy(QDir(host->directory).filePath("scene.tnz"));
  if (!directory.isValid() || !copy.open(QIODevice::WriteOnly) ||
      copy.write(sceneBytes) != sceneBytes.size() || !copy.flush())
    throw Error("io_error", "Cannot write the private scene copy.");
  copy.close();
  host->process    = std::make_unique<QProcess>();
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.remove("GROUNDZERO_AUTOMATION_CONTROL_FD");
  environment.insert("GROUNDZERO_SCENE_HOST_PARENT",
                     QString::number(QCoreApplication::applicationPid()));
  host->process->setProcessEnvironment(environment);
  host->process->setWorkingDirectory(host->directory);
  host->process->setProcessChannelMode(QProcess::SeparateChannels);
  host->process->setStandardErrorFile(QProcess::nullDevice());
  host->process->setProgram(QCoreApplication::applicationFilePath());
  host->process->setArguments(
      hostArguments(context, host->directory, documentId));
  host->process->start();
  if (!host->process->waitForStarted(SceneHostOpenDeadlineMs))
    host->lostCause = "crashed";
  else
    bindHostLifetime(*host);
  return host;
}

void awaitHosts(const std::vector<QProcess*>& processes, int budgetMs) {
  QElapsedTimer elapsed;
  elapsed.start();
  for (QProcess* process : processes) {
    const int left = budgetMs - int(elapsed.elapsed());
    if (left <= 0) return;
    if (process->state() != QProcess::NotRunning)
      process->waitForFinished(left);
  }
}

// Drop what the worker holds for an ended host: its lifetime handle and its
// private copy.
void releaseHost(SceneHost& host) {
#ifdef _WIN32
  if (host.lifetime) CloseHandle(host.lifetime);
  host.lifetime = nullptr;
#endif
  if (!host.directory.isEmpty()) QDir(host.directory).removeRecursively();
}

void removeHost(SceneHost& host) {
  if (host.process && host.process->state() != QProcess::NotRunning) {
    host.process->closeWriteChannel();
    if (!host.process->waitForFinished(HostCloseGraceMs)) stopProcess(host);
  }
  releaseHost(host);
}

// ---------------------------------------------------------
// scene.open_read_only

QString admittedScene(Context& context, const QJsonObject& params) {
  const QString path =
      context.resolvePath(requiredString(params, "path"), false);
  const QFileInfo info(path);
  if (!info.exists() && !info.isSymLink())
    throw Error("not_found", "The scene file does not exist.",
                {{"path", path}});
  if (info.isSymLink())
    throw Error("path_denied", "Symbolic links are not allowed.");
  if (!info.isFile() || info.suffix().toLower() != "tnz")
    throw Error("unsupported_type", "Open a regular .tnz scene file.",
                {{"path", path}});
  if (info.size() > SceneFileLimitBytes)
    throw Error(
        "scene_limit", "The scene file exceeds 64 MiB.",
        {{"limit", "scene_file_bytes"}, {"value", double(info.size())}});
  return path;
}

ProjectEntry selectedProject(Context& context, const QString& projectId) {
  if (projectId == "standalone") return {};
  const auto found = registry(context).projects.constFind(projectId);
  if (found == registry(context).projects.constEnd())
    throw Error("not_found",
                "Unknown project_id; call project.list for this worker.",
                {{"project_id", projectId}, {"hint", "project.list"}});
  return found.value();
}

void requireCapacity(Context& context) {
  if (readOnlyDocumentCount(context) >= ReadOnlyDocumentLimit)
    throw Error("resource_limit",
                "Discard a read-only document before opening another "
                "(limit: 4).",
                {{"limit", "read_only_documents"}});
  if (context.documents().size() >= 32)
    throw Error("resource_limit",
                "Close a document before opening another (limit: 32).",
                {{"limit", "documents"}});
}

QString containingRoot(const Context& context, const QString& path) {
  QString result;
  for (const QString& root : readRoots(context))
    if (beneath(path, root) &&
        (result.isEmpty() || root.size() < result.size()))
      result = root;
  return result;
}

QString findScenesXml(const QString& scene, const QString& root) {
  QString folder = QFileInfo(scene).absolutePath();
  for (;;) {
    const QString candidate = QDir(folder).filePath("scenes.xml");
    const QFileInfo info(candidate);
    if (info.exists() || info.isSymLink()) return candidate;
    if (folder == root || !beneath(folder, root)) return QString();
    const QString parent = QFileInfo(folder).absolutePath();
    if (parent == folder) return QString();
    folder = parent;
  }
}

QByteArray readSource(const QString& path, qint64 limit, const QString& code) {
  QFile file;
  const OpenResult opened = openApprovedFile(file, path);
  if (opened == OpenResult::NotRegular || !QFileInfo(path).isFile())
    throw Error(code, "A source file is missing.",
                {{"cause", "missing"}, {"path", path}});
  if (file.size() > limit)
    throw Error(code, "A source file exceeds its size limit.",
                {{"cause", "too_large"}, {"path", path}});
  if (opened != OpenResult::Opened)
    throw Error("io_error", "A source file cannot be read.", {{"path", path}});
  const QByteArray bytes = file.read(limit + 1);
  if (bytes.size() > limit)
    throw Error(code, "A source file exceeds its size limit.",
                {{"cause", "too_large"}, {"path", path}});
  return bytes;
}

QString scenesXmlRevision(const QString& path) {
  const QFileInfo info(path);
  if (path.isEmpty() || info.isSymLink() || !info.isFile() ||
      info.size() > ScenesXmlLimitBytes)
    return QString();
  QFile file;
  if (openApprovedFile(file, path) != OpenResult::Opened) return QString();
  return fileRevision(file.read(ScenesXmlLimitBytes + 1));
}

void rememberNativeProject(Context& context, const QJsonObject& source) {
  const QString id   = source.value("native_project_id").toString();
  const QString file = source.value("native_project_file").toString();
  if (id.isEmpty() || file.isEmpty() || id != projectIdForFile(file)) return;
  auto& projects = registry(context).projects;
  if (projects.contains(id) || projects.size() >= MaxRegistered) return;
  const QString folder = QFileInfo(file).absolutePath();
  projects.insert(id, {file, folder, QFileInfo(folder).fileName()});
}

QJsonObject loadParams(Context& context, const QString& scene,
                       const ProjectEntry& project, const QJsonObject& params,
                       const QByteArray& sceneBytes) {
  const QString scenesXml =
      findScenesXml(scene, containingRoot(context, scene));
  QJsonObject load{
      {"scene_path", scene},
      {"scene_revision", fileRevision(sceneBytes)},
      {"project_id", params.value("project_id")},
      {"binding", params.contains("project_binding")
                      ? params.value("project_binding")
                      : QJsonValue("must_match_native")},
      {"scenes_xml", scenesXml},
      {"scenes_xml_revision", scenesXmlRevision(scenesXml)},
      {"worker_workspace", context.workspace()},
      {"read_roots", QJsonArray::fromStringList(readRoots(context))},
      {"project_file", project.file},
      {"project_revision", QString()}};
  if (!project.file.isEmpty())
    load.insert("project_revision",
                fileRevision(readSource(project.file, ProjectFileLimitBytes,
                                        "invalid_project")));
  return load;
}

QJsonObject openHost(Context& context, SceneHost& host, QJsonObject load) {
  load.insert("scene_copy", QDir(host.directory).filePath("scene.tnz"));
  try {
    if (host.lost()) hostFailed(host, host.documentId);
    return exchange(context, host, "scene_host.load", load,
                    SceneHostOpenDeadlineMs, "read")
        .toObject();
  } catch (const Error& error) {
    removeHost(host);
    if (error.code == "project_mismatch")
      rememberNativeProject(context, error.details);
    if (error.code != "scene_host_failed") throw;
    throw Error("invalid_scene",
                "The scene host failed while opening the scene.",
                {{"cause", error.details.value("cause")}});
  }
}

QJsonValue openReadOnly(Context& context, const QJsonObject& params) {
  const QString scene        = admittedScene(context, params);
  const QString projectId    = requiredString(params, "project_id");
  const ProjectEntry project = selectedProject(context, projectId);
  requireCapacity(context);
  const QByteArray sceneBytes =
      readSource(scene, SceneFileLimitBytes, "scene_limit");
  const QJsonObject load =
      loadParams(context, scene, project, params, sceneBytes);
  const QString documentId =
      "document_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  context.markFailureRecorded();
  QElapsedTimer timer;
  timer.start();
  auto host          = startHost(context, documentId, sceneBytes);
  QJsonObject result = openHost(context, *host, load);
  QJsonObject report = result.value("load").toObject();
  report.insert("elapsed_ms",
                std::min<qint64>(timer.elapsed(), SceneHostOpenDeadlineMs));
  result.insert("load", report);
  rememberNativeProject(context, result);
  result.remove("native_project_id");
  result.remove("native_project_file");
  context.markMutationStarted();
  context.addReadOnlyDocument(std::make_unique<ToonzScene>(), documentId,
                              std::make_shared<ReadOnlyScene>());
  registry(context).hosts.emplace(documentId, std::move(host));
  return result;
}

// ------------------------------------------------- project.inspect, .scenes

constexpr int MaxSceneDepth   = 8;
constexpr int MaxScenePage    = 128;
constexpr int MaxSceneEntries = 20000;

ProjectEntry grantedProject(Context& context, const QJsonObject& params) {
  const QString id = requiredString(params, "project_id");
  if (id == "standalone")
    throw Error("invalid_params", "standalone has no project file.");
  return selectedProject(context, id);
}

// The worker's view of a decoded project folder: authority within the roots,
// and existence only for folders inside them.
QJsonObject grantedFolder(Context& context, QJsonObject folder) {
  const QString decoded = folder.take("decoded").toString();
  QString authority     = "unresolved";
  QJsonValue resolved, exists;
  if (folder.value("scene_dependent").toBool()) {
    authority = "scene_dependent";
  } else if (QDir::isAbsolutePath(decoded)) {
    authority = "outside_roots";
    try {
      const QString allowed =
          context.resolvePath(QDir::cleanPath(decoded), false);
      authority =
          beneath(allowed, context.workspace()) ? "workspace" : "input_root";
      resolved = allowed;
      exists   = QFileInfo(allowed).exists();
    } catch (const Error&) {
    }
  }
  folder.insert("resolved_path", resolved);
  folder.insert("authority", authority);
  folder.insert("exists", exists);
  return folder;
}

// Parse a granted project file in a parse helper, hashing it before and after.
QJsonObject parsedProject(Context& context, const ProjectEntry& project,
                          QString& revision) {
  revision = fileRevision(
      readSource(project.file, ProjectFileLimitBytes, "invalid_project"));
  QJsonObject summary = parseSharedFile(
      context, "project", project.file.toUtf8(), "invalid_project");
  if (fileRevision(readSource(project.file, ProjectFileLimitBytes,
                              "invalid_project")) != revision)
    throw Error("source_changed", "The project file changed while it was read.",
                {{"paths", QJsonArray{project.file}}, {"count", 1}});
  QJsonArray folders;
  for (const QJsonValue& folder : summary.value("folders").toArray())
    folders.append(grantedFolder(context, folder.toObject()));
  summary.insert("folders", folders);
  return summary;
}

struct SceneScan final {
  QElapsedTimer timer;
  int entries  = 0;
  qint64 links = 0;
  QStringList reasons;
  QJsonArray rows;

  void incomplete(const QString& reason) {
    if (!reasons.contains(reason)) reasons.append(reason);
  }
};

QJsonObject sceneRow(const EntryInfo& entry, const QString& root) {
  const bool folder = entry.isDir();
  return {
      {"path", entry.absoluteFilePath()},
      {"relative_path", QDir(root).relativeFilePath(entry.absoluteFilePath())},
      {"kind", folder ? "folder" : "scene"},
      {"size_bytes", folder ? QJsonValue() : QJsonValue(double(entry.size()))},
      {"modified", entry.lastModified().toUTC().toString(Qt::ISODate)}};
}

void scanScenes(SceneScan& scan, const QString& folder, const QString& root,
                int depth) {
  bool readable  = false;
  bool truncated = false;
  const QVector<EntryInfo> entries =
      listedEntries(folder, readable, truncated, MaxSceneEntries + 1);
  if (!readable) return scan.incomplete("unreadable");
  if (truncated) scan.incomplete("entry_limit");
  for (const EntryInfo& entry : entries) {
    if (++scan.entries > MaxSceneEntries) return scan.incomplete("entry_limit");
    if (scan.timer.elapsed() > MaxScanMs) return scan.incomplete("time_limit");
    if (entry.fileName().startsWith('.')) continue;
    if (entry.isSymLink()) {
      ++scan.links;
      scan.incomplete("skipped_links");
      continue;
    }
    if (entry.isFile() && entry.suffix().toLower() == "tnz")
      scan.rows.append(sceneRow(entry, root));
    if (!entry.isDir()) continue;
    scan.rows.append(sceneRow(entry, root));
    if (depth + 1 > MaxSceneDepth)
      scan.incomplete("depth_limit");
    else
      scanScenes(scan, entry.absoluteFilePath(), root, depth + 1);
  }
}

QString scenesFolder(const QJsonObject& project) {
  for (const QJsonValue& value : project.value("folders").toArray()) {
    const QJsonObject folder = value.toObject();
    if (folder.value("alias") == "scenes" && folder.value("exists").toBool())
      return folder.value("resolved_path").toString();
  }
  return QString();
}

QString sceneSubfolder(Context& context, const QString& root,
                       const QJsonObject& params) {
  if (!params.contains("folder")) return root;
  const QString relative = requiredString(params, "folder");
  if (QDir::isAbsolutePath(relative) || relative.contains('\\') ||
      relative.split('/').contains(".."))
    throw Error("invalid_path",
                "folder is a forward-slash path inside the scenes folder.");
  const QString folder =
      context.resolvePath(QDir(root).filePath(relative), false);
  if (!QFileInfo(folder).isDir())
    throw Error("not_found", "The folder does not exist.", {{"path", folder}});
  return folder;
}

QJsonObject scenesPage(const QJsonObject& params, const SceneScan& scan,
                       const QString& id, const QJsonValue& root) {
  const QString revision =
      "listing:" +
      fileRevision(QJsonDocument(scan.rows).toJson(QJsonDocument::Compact))
          .mid(7);
  if (params.contains("expected_listing_revision") &&
      params.value("expected_listing_revision").toString() != revision)
    throw Error("revision_conflict", "The scene listing changed.",
                {{"scope", "listing"}, {"listing_revision", revision}});
  const int offset = integerParam(params, "offset", 0, MaxSceneEntries, 0);
  const int limit  = integerParam(params, "limit", 1, MaxScenePage, 32);
  if (offset > scan.rows.size())
    throw Error("invalid_params", "offset exceeds the discovered entries.");
  const int end = std::min(offset + limit, int(scan.rows.size()));
  QJsonArray page;
  for (int index = offset; index < end; ++index)
    page.append(scan.rows.at(index));
  return {
      {"project_id", id},
      {"scenes_folder", root},
      {"items", page},
      {"listing_revision", revision},
      {"offset", offset},
      {"limit", limit},
      {"discovered", int(scan.rows.size())},
      {"next_offset", end < scan.rows.size() ? QJsonValue(end) : QJsonValue()},
      {"scan",
       QJsonObject{{"complete", scan.reasons.isEmpty()},
                   {"reasons", QJsonArray::fromStringList(scan.reasons)},
                   {"entries_visited", scan.entries},
                   {"skipped_links", double(scan.links)}}}};
}

QJsonValue listScenes(Context& context, const QJsonObject& params) {
  const ProjectEntry project = grantedProject(context, params);
  QString revision;
  const QString root = scenesFolder(parsedProject(context, project, revision));
  SceneScan scan;
  scan.timer.start();
  if (root.isEmpty()) {
    scan.incomplete("unreadable");
    return scenesPage(params, scan, params.value("project_id").toString(),
                      QJsonValue());
  }
  const QString folder = sceneSubfolder(context, root, params);
  scanScenes(scan, folder, root, 0);
  return scenesPage(params, scan, params.value("project_id").toString(), root);
}

// ------------------------------------------------------------------ routing

const QSet<QString>& forwardedTools() {
  static const QSet<QString> tools = {
      "scene.summary",       "camera.inspect",
      "level.inspect",       "cast.list",
      "cast.usage",          "scene.dependencies",
      "scene.validate",      "fx.graph",
      "render.preview",      "render.submit",
      "render.preflight",    "scene.inventory",
      "scene.check.submit",  "drawing.analysis.submit",
      "scene.source_status", "column.cells",
      "drawing.inspect",     "palette.inspect",
      "palette.pages",       "fx.parameter.get",
      "animation.channels",  "animation.keys",
      "animation.sample",    "vector.inspect",
      "vector.structure",    "vector.opacity.inspect",
      "raster.read_pixels"};
  return tools;
}

// Reads that decode drawings: a changed level file refuses them.
const QSet<QString>& decodingTools() {
  static const QSet<QString> tools = {
      "drawing.inspect",    "vector.inspect",
      "vector.structure",   "vector.opacity.inspect",
      "raster.read_pixels", "drawing.analysis.submit"};
  return tools;
}

[[noreturn]] void refuseReadOnly(const QString& method,
                                 const QJsonObject& descriptor) {
  // A tool whose declared effect is to start a job reads the document and
  // changes nothing, so a read-only document refuses it as not yet qualified.
  const bool startsJob =
      descriptor.value("effects").toObject().value("job").toString() ==
      "starts";
  const bool write = !descriptor.value("annotations")
                          .toObject()
                          .value("readOnlyHint")
                          .toBool() &&
                     !startsJob;
  if (write)
    throw Error("read_only_document",
                "The document is read-only; this tool changes a document.",
                {{"reason", "mutation"}, {"tool", method}});
  const QString toolset = descriptor.value("toolset").toString();
  QJsonValue slice;
  if (toolset == "rigging" || toolset == "audio" || toolset == "storyboard")
    slice = 3;
  throw Error(
      "read_only_document",
      "This tool is not yet qualified on read-only documents.",
      {{"reason", "not_qualified"}, {"tool", method}, {"slice", slice}});
}

SceneHost* hostForDocument(Context& context, const QString& documentId) {
  if (!context.sceneHosts) return nullptr;
  const auto found = context.sceneHosts->hosts.find(documentId);
  return found == context.sceneHosts->hosts.end() ? nullptr
                                                  : found->second.get();
}

// The discarded document's jobs expire with its host; their ids keep
// answering unknown_job with reason expired.
void forgetRoutes(Context& context, const QString& documentId) {
  auto& state = registry(context);
  QStringList expired;
  for (auto it = state.jobs.begin(); it != state.jobs.end();) {
    if (it.value() != documentId) {
      ++it;
      continue;
    }
    expired.append(it.key());
    state.records.remove(it.key());
    it = state.jobs.erase(it);
  }
  jobRegistry(context).expire(expired);
  for (auto it = state.artifacts.begin(); it != state.artifacts.end();)
    it = it.value() == documentId ? state.artifacts.erase(it) : std::next(it);
}

QJsonValue discardReadOnly(Context& context, SceneHost& host) {
  const QString id = host.documentId;
  try {
    if (!host.lost())
      exchange(context, host, "scene_host.close", {}, SceneHostReadDeadlineMs,
               "read");
  } catch (const Error& error) {
    if (error.code != "scene_host_failed") throw;
  }
  context.markMutationStarted();
  removeHost(host);
  forgetRoutes(context, id);
  context.removeDocument(id);
  registry(context).hosts.erase(id);
  return QJsonObject{{"document_id", id}, {"discarded", true}};
}

bool terminalStatus(const QString& status) {
  return status == "completed" || status == "failed" || status == "canceled" ||
         status == "deadline_exceeded";
}

// The job record in a job tool's result: the result itself, or under "job".
// An analysis job's result page names the job but is not its record.
QJsonObject jobRecordIn(const QJsonObject& result) {
  const QJsonObject nested = result.value("job").toObject();
  const QJsonObject record = nested.isEmpty() ? result : nested;
  return record.contains("lane") && record.contains("job_id") ? record
                                                              : QJsonObject();
}

// A host that ended cannot finish its jobs: the last record of each one that
// was still running reads failed, stopped by force with the host.
QJsonObject lostJobRecord(Context& context, QJsonObject record) {
  if (record.value("terminal").toBool()) return record;
  record.insert("status", "failed");
  record.insert("terminal", true);
  record.insert("failure",
                QJsonObject{{"code", "scene_host_failed"},
                            {"phase", "executor"},
                            {"message", "The document's scene host ended."}});
  record.insert("termination",
                QJsonObject{{"mode", "forced"},
                            {"forced_processes", QJsonArray{"scene_host"}}});
  QJsonObject timing = record.value("timing").toObject();
  timing.insert("ended_ms", double(jobRegistry(context).elapsedMs()));
  record.insert("timing", timing);
  return record;
}

void recordRoutes(Context& context, SceneHost& host, const QJsonValue& value) {
  const QJsonObject result = value.toObject();
  const QJsonObject record = jobRecordIn(result);
  const QString job =
      result.value("job_id").toString(record.value("job_id").toString());
  if (!record.isEmpty()) {
    registry(context).records.insert(job, record);
    jobRegistry(context).note(job);
  }
  if (!job.isEmpty()) {
    registry(context).jobs.insert(job, host.documentId);
    const bool render = registry(context).records.value(job).value("lane") ==
                        laneText(JobLane::Render);
    if (render && (job == host.lastJob || !host.renderActive)) {
      host.lastJob      = job;
      host.renderActive = !terminalStatus(result.value("status").toString());
    }
  }
  for (const QJsonValue& artifact : result.value("artifacts").toArray())
    registry(context).artifacts.insert(
        artifact.toObject().value("artifact_id").toString(), host.documentId);
}

QJsonValue forward(Context& context, SceneHost& host, const QString& method,
                   const QJsonObject& params) {
  const bool render  = method.startsWith("render.") ||
                       method.startsWith("job.") || method == "artifact.read";
  const int deadline = method == "render.preview" || method == "render.submit"
                           ? SceneHostOpenDeadlineMs
                           : SceneHostReadDeadlineMs;
  try {
    const QJsonValue result = exchange(context, host, method, params, deadline,
                                       render ? "render" : "read");
    recordRoutes(context, host, result);
    return result;
  } catch (Error& error) {
    // A lost host's jobs report as failed.
    const QString job = params.value("job_id").toString();
    const auto known  = registry(context).records.constFind(job);
    if (error.code == "scene_host_failed" &&
        (method == "job.status" || method == "job.cancel") &&
        known != registry(context).records.constEnd())
      return lostJobRecord(context, known.value());
    if (error.code == "scene_host_failed" && !job.isEmpty()) {
      error.details.insert("job_id", job);
      error.details.insert("status", "failed");
    }
    throw;
  }
}

struct RenderCapacity final {
  QString active;
  int retained = 0;
};

// One active render job and 32 retained jobs per worker, hosts included.
RenderCapacity renderCapacity(Context& context) {
  RenderCapacity capacity{activeRenderJob(context), renderJobCount(context)};
  if (!context.sceneHosts) return capacity;
  for (const QJsonObject& record : context.sceneHosts->records)
    if (record.value("lane") == laneText(JobLane::Render)) ++capacity.retained;
  for (auto& entry : context.sceneHosts->hosts) {
    SceneHost& host = *entry.second;
    if (!capacity.active.isEmpty() || !host.renderActive || host.lost())
      continue;
    try {
      recordRoutes(
          context, host,
          exchange(context, host, "job.status", {{"job_id", host.lastJob}},
                   SceneHostReadDeadlineMs, "render"));
    } catch (const Error& error) {
      if (error.code != "scene_host_failed") throw;
    }
    if (host.renderActive && !host.lost()) capacity.active = host.lastJob;
  }
  return capacity;
}

void requireRenderCapacity(Context& context) {
  const RenderCapacity capacity = renderCapacity(context);
  if (!capacity.active.isEmpty())
    throw Error("render_busy",
                "Wait for the active render job to finish or cancel it.",
                {{"job_id", capacity.active}});
  if (capacity.retained >= 32)
    throw Error("render_limit",
                "This worker has reached its render job limit.");
}

QJsonObject capacityCheck(const QString& id, const QString& code,
                          const QJsonObject& details) {
  return {{"id", id},
          {"status", code.isEmpty() ? "pass" : "fail"},
          {"code", code.isEmpty() ? QJsonValue() : QJsonValue(code)},
          {"details", details}};
}

// A host sees only its own jobs; the lane and retention checks of a
// read-only preflight are the worker's, hosts included.
QJsonObject withWorkerCapacity(Context& context, QJsonObject report) {
  const RenderCapacity capacity = renderCapacity(context);
  QJsonArray checks;
  QString firstFailure;
  for (const QJsonValue& value : report.value("checks").toArray()) {
    QJsonObject check = value.toObject();
    const QString id  = check.value("id").toString();
    if (id == "lane")
      check =
          capacity.active.isEmpty()
              ? capacityCheck(id, {}, {})
              : capacityCheck(id, "render_busy", {{"job_id", capacity.active}});
    else if (id == "retention")
      check =
          capacity.retained < 32
              ? capacityCheck(id, {}, {})
              : capacityCheck(id, "render_limit", {{"max_retained_jobs", 32}});
    if (firstFailure.isEmpty() && check.value("status") == "fail")
      firstFailure = id;
    checks.append(check);
  }
  report.insert("checks", checks);
  report.insert("first_failure", firstFailure.isEmpty()
                                     ? QJsonValue(QJsonValue::Null)
                                     : QJsonValue(firstFailure));
  report.insert("ready", firstFailure.isEmpty());
  return report;
}

SceneHost* hostForRequest(Context& context, const QJsonObject& params) {
  if (!context.sceneHosts) return nullptr;
  const auto& state = *context.sceneHosts;
  QString documentId;
  if (params.value("job_id").isString())
    documentId = state.jobs.value(params.value("job_id").toString());
  if (documentId.isEmpty() && params.value("artifact_id").isString())
    documentId = state.artifacts.value(params.value("artifact_id").toString());
  return documentId.isEmpty() ? nullptr : hostForDocument(context, documentId);
}

}  // namespace

// Every host's input is closed first, so all of them end together within one
// budget; any still running is then killed, again together.
SceneHostRegistry::~SceneHostRegistry() {
  std::vector<QProcess*> running;
  for (auto& entry : hosts)
    if (entry.second->process &&
        entry.second->process->state() != QProcess::NotRunning) {
      entry.second->process->closeWriteChannel();
      running.push_back(entry.second->process.get());
    }
  awaitHosts(running, HostCloseBudgetMs);
  for (QProcess* process : running)
    if (process->state() != QProcess::NotRunning) process->kill();
  awaitHosts(running, HostKillBudgetMs);
  for (auto& entry : hosts) releaseHost(*entry.second);
}

void refuseReadOnlyEarly(Context& context, const QString& method,
                         const QJsonObject& params,
                         const QJsonObject& descriptor) {
  if (!params.value("document_id").isString() || method == "scene.discard" ||
      method == "job.list" || forwardedTools().contains(method))
    return;
  const QString documentId = params.value("document_id").toString();
  try {
    if (!context.document(documentId).readOnly) return;
  } catch (const Error&) {
    return;
  }
  refuseReadOnly(method, descriptor);
}

QJsonObject hostJobRecord(Context& context, const QString& id) {
  if (!context.sceneHosts) return {};
  auto& state              = *context.sceneHosts;
  const QString documentId = state.jobs.value(id);
  SceneHost* host =
      documentId.isEmpty() ? nullptr : hostForDocument(context, documentId);
  if (!host) return {};
  try {
    if (!host->lost())
      return forward(context, *host, "job.status", {{"job_id", id}}).toObject();
  } catch (const Error& error) {
    if (error.code != "scene_host_failed") throw;
  }
  return lostJobRecord(context, state.records.value(id));
}

// One active analysis job and 32 retained per worker, hosts included. A host
// job last seen running is asked again, which its host answers at once.
QJsonValue readFromSceneHost(Context& context, const QString& documentId,
                             const QString& method, const QJsonObject& params) {
  SceneHost* host = hostForDocument(context, documentId);
  if (!host) throw Error("internal_error", "The document has no scene host.");
  return forward(context, *host, method, params);
}

void requireAnalysisCapacity(Context& context) {
  QString active;
  int retained = 0;
  if (context.sceneHosts) {
    const QString analysis = laneText(JobLane::Analysis);
    QStringList running;
    for (auto it = context.sceneHosts->records.constBegin();
         it != context.sceneHosts->records.constEnd(); ++it) {
      if (it.value().value("lane") != analysis) continue;
      ++retained;
      if (!it.value().value("terminal").toBool()) running.append(it.key());
    }
    for (const QString& job : running) {
      if (!active.isEmpty()) break;
      const QJsonObject record = hostJobRecord(context, job);
      if (!record.isEmpty() && !record.value("terminal").toBool()) active = job;
    }
  }
  jobRegistry(context).admitAnalysis(active, retained);
}

bool routeToSceneHost(Context& context, const QString& method,
                      const QJsonObject& params, const QJsonObject& descriptor,
                      QJsonValue& result) {
  // job.list filters by document; it reads the jobs of every host itself.
  if (method == "job.list") return false;
  const QString documentId = params.value("document_id").toString();
  Document* document       = nullptr;
  if (!documentId.isEmpty()) {
    document = &context.document(documentId);
    if (!document->readOnly) document = nullptr;
  }
  if (!context.sceneHostDocument.isEmpty()) {
    if (!document) return false;
    if (!forwardedTools().contains(method)) refuseReadOnly(method, descriptor);
#ifdef BUILD_UI_PORT_TESTS
    if (context.childFault == "read-hang" && method == "scene.summary")
      injectChildFault("hang");
#endif
    if (decodingTools().contains(method) && params.value("level_id").isString())
      requireUnchangedLevel(
          *document, document->level(params.value("level_id").toString()));
    return false;
  }
  if (context.sceneHosts &&
      (method == "render.preview" || method == "render.submit" ||
       method == "assistance.neural.preview"))
    requireRenderCapacity(context);
  if (context.sceneHosts && document && method == "drawing.analysis.submit")
    requireAnalysisCapacity(context);
  if (context.sceneHosts && document && method == "scene.check.submit") {
    requireAnalysisCapacity(context);
    // The worker decodes media in helpers it starts; a host starts none.
    if (params.value("check") == "media_decode") return false;
  }
  if (document) {
    SceneHost* host = hostForDocument(context, documentId);
    if (!host) throw Error("internal_error", "The document has no scene host.");
    if (method == "scene.discard") {
      result = discardReadOnly(context, *host);
      return true;
    }
    if (!forwardedTools().contains(method)) refuseReadOnly(method, descriptor);
    result = forward(context, *host, method, params);
    if (method == "render.preflight")
      result = withWorkerCapacity(context, result.toObject());
    return true;
  }
  SceneHost* host = hostForRequest(context, params);
  if (!host) return false;
  result = forward(context, *host, method, params);
  return true;
}

int readOnlyDocumentCount(Context& context) {
  return context.sceneHosts ? int(context.sceneHosts->hosts.size()) : 0;
}

QJsonArray documentRows(Context& context) {
  QJsonArray rows;
  for (const QJsonValue& value : context.documents()) {
    QJsonObject row = value.toObject();
    SceneHost* host =
        hostForDocument(context, row.value("document_id").toString());
    row.insert("host_state", !host          ? QJsonValue()
                             : host->lost() ? QJsonValue("lost")
                                            : QJsonValue("ready"));
    rows.append(row);
  }
  return rows;
}

QJsonObject readOnlyCapabilities(Context&) {
  return {{"available", true},
          {"max_documents", ReadOnlyDocumentLimit},
          {"host_memory_bytes", double(SceneHostMemoryBytes)},
          {"open_deadline_ms", SceneHostOpenDeadlineMs},
          {"read_deadline_ms", SceneHostReadDeadlineMs},
          {"profile", readOnlyProfileName()}};
}

void closeSceneHosts(Context& context) { context.sceneHosts.reset(); }

QJsonValue inspectGrantedProject(Context& context, const QString& projectId) {
  const ProjectEntry project =
      grantedProject(context, {{"project_id", projectId}});
  QString revision;
  const QJsonObject summary = parsedProject(context, project, revision);
  return QJsonObject{{"kind", "granted_project"},
                     {"project_id", projectId},
                     {"name", summary.value("name").toString().isEmpty()
                                  ? project.name
                                  : summary.value("name").toString()},
                     {"folder", project.folder},
                     {"project_file", project.file},
                     {"file_revision", revision},
                     {"format", projectFormat(project.file)},
                     {"version", summary.value("version")},
                     {"file_path_rules", summary.value("file_path_rules")},
                     {"folders", summary.value("folders")},
                     {"scene_defaults", summary.value("scene_defaults")},
                     {"alternates", QJsonArray::fromStringList(alternateFiles(
                                        project.folder, project.file))}};
}

QJsonArray sceneAccessOperations() {
  const auto text = [](int maximum) {
    return QJsonObject{
        {"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
  };
  const auto integer = [](int minimum, int maximum) {
    return QJsonObject{
        {"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
  };
  QJsonObject list = operation(
      "project.list",
      "List the read roots and the native projects found under them (project "
      "files are identified, hashed and never parsed). Use a project_id, or "
      "standalone, to open a scene with scene.open_read_only.",
      {{"folder", text(4096)},
       {"depth", integer(1, MaxScanDepth)},
       {"offset", integer(0, MaxProjects)},
       {"limit", integer(1, 64)},
       {"expected_listing_revision", text(80)}},
      {});
  const QJsonObject open = operation(
      "scene.open_read_only",
      "Open a native .tnz scene from a read root as a read-only document, "
      "loaded and rendered only inside its own scene host process. Requires "
      "an explicit project_id (from project.list) or standalone; the scene's "
      "own scenes.xml binding must match unless project_binding is override. "
      "Nothing on disk changes.",
      {{"path", text(4096)},
       {"project_id", text(64)},
       {"project_binding",
        QJsonObject{{"type", "string"},
                    {"enum", QJsonArray{"must_match_native", "override"}}}},
       {"operation_id", text(128)}},
      {"path", "project_id", "operation_id"}, true);
  const QJsonObject status = operation(
      "scene.source_status",
      "Re-check every file pinned when a read-only document was opened and "
      "report whether the source on disk changed.",
      {{"document_id", text(128)}, {"expected_revision", text(80)}},
      {"document_id", "expected_revision"});
  const QJsonObject scenes = operation(
      "project.scenes",
      "List .tnz scene files and folders under a project's scenes folder, "
      "paged. Nothing is parsed.",
      {{"project_id", text(64)},
       {"folder", text(1024)},
       {"offset", integer(0, MaxSceneEntries)},
       {"limit", integer(1, MaxScenePage)},
       {"expected_listing_revision", text(80)}},
      {"project_id"});
  return {list, open, status, scenes};
}

QJsonValue executeSceneAccess(Context& context, const QString& method,
                              const QJsonObject& params) {
  if (method == "scene.source_status") {
    Document& document =
        context.document(requiredString(params, "document_id"));
    if (requiredString(params, "expected_revision") != document.revisionText())
      throw Error("stale_revision", "Refresh scene.summary before continuing.",
                  {{"document_id", document.id},
                   {"revision", document.revisionText()}});
    if (!document.readOnly)
      throw Error("unsupported_target",
                  "An editable document has no pinned source files.");
    return sourceStatus(document);
  }
  if (!context.sceneHostDocument.isEmpty())
    throw Error("method_not_found",
                "A scene host does not open other documents.");
  if (method == "project.list") return listProjects(context, params);
  if (method == "project.scenes") return listScenes(context, params);
  if (method == "scene.open_read_only") return openReadOnly(context, params);
  throw Error("method_not_found", "Unknown scene access operation.");
}

}  // namespace GroundzeroAutomation
