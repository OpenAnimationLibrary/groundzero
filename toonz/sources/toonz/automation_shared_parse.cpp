#include "automation_shared_parse.h"
#include "automation_media_decode.h"
#include "automation_runtime.h"
#include "automation_scene_host.h"
#include "librarycommand.h"

#include "tcolorstyles.h"
#include "texception.h"
#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "toonz/levelset.h"
#include "toonz/poselibrary.h"
#include "toonz/studiopalette.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevel.h"
#include "toonz/txshleveltypes.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <typeinfo>

#ifdef _WIN32
#include <qt_windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#else
#include <unistd.h>
#endif

namespace GroundzeroAutomation {
namespace {

constexpr qint64 MaxHelperOutput = 8 * 1024 * 1024;
constexpr int MaxStyles          = 4096;
constexpr int MaxPages           = 128;
constexpr int MaxLevels          = 256;
constexpr int MaxColumns         = 4096;
constexpr int MaxFrames          = 1000000;
constexpr int MaxChildSheets     = 128;
constexpr int MaxPoses           = 1024;
constexpr int MaxPoseEntries     = 4096;
constexpr int MaxThumbnailBytes  = 1024 * 1024;
constexpr int TimeoutExit        = 124;
constexpr int MemoryExit         = 125;

[[noreturn]] void outOfBounds(const QString& message) {
  throw Error("resource_limit", message);
}

QString extension(const QString& kind) {
  if (kind == "studio_palette") return "tpl";
  if (kind == "template") return "tnz";
  if (kind == "project") return "path";
  return "tposes";
}

QString text(const std::wstring& value) {
  return QString::fromStdWString(value);
}

QJsonObject styleSummary(TPalette& palette, int id) {
  TColorStyle* style   = palette.getStyle(id);
  TPalette::Page* page = palette.getStylePage(id);
  const QString origin = text(style->getOriginalName());
  const QString global = text(style->getGlobalName());
  QJsonObject summary{
      {"style_id", id},
      {"name", text(style->getName())},
      {"native_tag", style->getTagId()},
      {"solid", typeid(*style) == typeid(TSolidColorStyle)},
      {"keyframe_count", palette.getKeyframeCount(id)},
      {"page_index", page ? QJsonValue(page->getIndex()) : QJsonValue()},
      {"global_name", global},
      {"original_name", origin},
      {"edited", style->getIsEditedFlag()}};
  if (style->hasMainColor()) {
    const TPixel32 color = style->getMainColor();
    summary.insert(
        "color",
        QJsonObject{
            {"r", color.r}, {"g", color.g}, {"b", color.b}, {"a", color.m}});
  }
  return summary;
}

QJsonObject paletteSummary(const TFilePath& path) {
  std::unique_ptr<TPalette> palette(StudioPalette::load(path));
  if (!palette) throw Error("rejected", "The studio palette cannot be read.");
  if (palette->getStyleCount() > MaxStyles ||
      palette->getPageCount() > MaxPages)
    outOfBounds("The palette exceeds 4096 styles or 128 pages.");
  QJsonArray pages;
  for (int index = 0; index < palette->getPageCount(); ++index) {
    const TPalette::Page* page = palette->getPage(index);
    QJsonArray ids;
    for (int slot = 0; slot < page->getStyleCount(); ++slot)
      ids.append(page->getStyleId(slot));
    pages.append(
        QJsonObject{{"name", text(page->getName())}, {"style_ids", ids}});
  }
  QJsonArray styles;
  for (int id = 0; id < palette->getStyleCount(); ++id)
    styles.append(styleSummary(*palette, id));
  return {{"global_name", text(palette->getGlobalName())},
          {"locked", palette->isLocked()},
          {"cleanup_palette", palette->isCleanupPalette()},
          {"animated", palette->isAnimated()},
          {"color_model", !palette->getRefImgPath().isEmpty()},
          {"pages", pages},
          {"styles", styles}};
}

QString levelFormat(TXshLevel* level) {
  if (!level->getSimpleLevel()) return "unsupported";
  switch (level->getType()) {
  case PLI_XSHLEVEL:
    return "vector";
  case TZP_XSHLEVEL:
    return "toonzRaster";
  case OVL_XSHLEVEL:
    return "rgba8";
  default:
    return "unsupported";
  }
}

QJsonObject templateSummary(Context& context, const TFilePath& path) {
  auto scene = std::make_unique<ToonzScene>();
  scene->setProject(context.project());
  scene->loadTnzFile(path);
  scene->getXsheet()->updateFrameCount();
  TLevelSet* set = scene->getLevelSet();
  QJsonArray levels;
  int children = 0, others = 0;
  for (int index = 0; index < set->getLevelCount(); ++index) {
    TXshLevel* level = set->getLevel(index);
    if (level->getChildLevel()) {
      ++children;
      continue;
    }
    const QString reference = text(level->getPath().getWideString());
    if (reference.isEmpty()) {
      ++others;
      continue;
    }
    if (levels.size() == MaxLevels)
      outOfBounds("The template has >256 levels.");
    levels.append(QJsonObject{{"name", text(level->getName())},
                              {"format", levelFormat(level)},
                              {"reference", reference}});
  }
  const int frames  = scene->getFrameCount();
  const int columns = scene->getXsheet()->getColumnCount();
  if (frames > MaxFrames || columns > MaxColumns || children > MaxChildSheets)
    outOfBounds("The template exceeds its frame, column or sheet bounds.");
  return {{"frame_count", frames},
          {"column_count", columns},
          {"child_sheet_count", children},
          {"pathless_levels", others},
          {"unrecognized_scene_data", scene->hasUnrecognizedSceneData()},
          {"levels", levels}};
}

QJsonObject poseSummary(const TFilePath& path, int thumbnail) {
  PoseLibrary poses;
  if (!LibraryCmd::readPoseSet(path, poses))
    throw Error("rejected", "The pose set cannot be read.");
  if (poses.getPoseCount() > MaxPoses)
    outOfBounds("The pose set has more than 1024 poses.");
  QJsonArray rows;
  for (int index = 0; index < poses.getPoseCount(); ++index) {
    const PoseLibrary::Pose& pose = poses.getPose(index);
    if (int(pose.m_entries.size()) > MaxPoseEntries || pose.m_name.size() > 256)
      outOfBounds("A pose exceeds 4096 entries or a 256-character name.");
    rows.append(QJsonObject{{"name", pose.m_name},
                            {"entry_count", int(pose.m_entries.size())},
                            {"has_thumbnail", !pose.m_thumbnail.isEmpty()}});
  }
  QJsonObject summary{{"has_base", !poses.getBase().empty()}, {"poses", rows}};
  if (thumbnail >= 0 && thumbnail < poses.getPoseCount()) {
    const QByteArray& bytes = poses.getPose(thumbnail).m_thumbnail;
    if (bytes.size() > MaxThumbnailBytes)
      outOfBounds("The pose thumbnail exceeds 1 MiB.");
    if (!bytes.isEmpty())
      summary.insert("thumbnail", QString::fromLatin1(bytes.toBase64()));
  }
  return summary;
}

// The worker passes the real project file's path, not a copy: relative
// project folders resolve against the folder that holds it.
QJsonObject projectSummary(const TFilePath& input) {
  QFile file(input.getQString());
  if (!file.open(QIODevice::ReadOnly))
    throw Error("rejected", "The project path cannot be read.");
  return nativeProjectSummary(QString::fromUtf8(file.read(8192)));
}

QJsonObject summarize(Context& context, const QString& kind) {
  static const QRegularExpression shape(QRegularExpression::anchoredPattern(
      "(studio_palette|template|project|pose_set|pose_thumbnail:([0-9]{1,4})"
      ")"));
  const auto match = shape.match(kind);
  if (!match.hasMatch()) throw Error("rejected", "Unknown parse kind.");
  const TFilePath path(QDir(context.workspace())
                           .filePath("input." + extension(kind))
                           .toStdWString());
  if (kind == "studio_palette") return paletteSummary(path);
  if (kind == "template") return templateSummary(context, path);
  if (kind == "project") return projectSummary(path);
  const int thumbnail =
      match.captured(2).isEmpty() ? -1 : match.captured(2).toInt();
  return poseSummary(path, thumbnail);
}

QJsonObject helperReply(Context& context, const QString& kind) {
  try {
    return {{"status", "parsed"}, {"summary", summarize(context, kind)}};
  } catch (const Error& error) {
    return {{"status", error.code == "resource_limit" ? "limit" : "rejected"},
            {"message", error.message.left(512)}};
  } catch (const TException& error) {
    return {{"status", "rejected"},
            {"message", text(error.getMessage()).left(512)}};
  } catch (const std::exception& error) {
    return {{"status", "rejected"},
            {"message", QString::fromUtf8(error.what()).left(512)}};
  } catch (...) {
    return {{"status", "rejected"}, {"message", "Unrecognized native error."}};
  }
}

int helperDeadlineMs(const QString& kind) {
  return kind == "gl_probe" ? GraphicsProbeDeadlineMs : SharedParseDeadlineMs;
}

void startSelfWatchdog(int deadlineMs) {
  const qint64 process = QCoreApplication::applicationPid();
  std::thread([process, deadlineMs] {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (std::chrono::steady_clock::now() - start >
          std::chrono::milliseconds(deadlineMs + 5000))
        std::_Exit(TimeoutExit);
      if (processMemoryBytes(process) > SharedParseMemoryBytes)
        std::_Exit(MemoryExit);
    }
  }).detach();
}

struct HelperRun final {
  QByteArray output;
  QString cause;
  qint64 elapsedMs = 0;
};

QString exitCause(const QProcess& helper) {
  if (helper.exitStatus() == QProcess::CrashExit) return "crashed";
  if (helper.exitCode() == TimeoutExit) return "timeout";
  if (helper.exitCode() == MemoryExit) return "memory";
  return helper.exitCode() == 0 ? QString() : QString("crashed");
}

QString breach(const QProcess& helper, const QElapsedTimer& timer,
               qint64 output, int deadlineMs) {
  if (output > MaxHelperOutput) return "crashed";
  if (timer.elapsed() > deadlineMs) return "timeout";
  if (processMemoryBytes(helper.processId()) > SharedParseMemoryBytes)
    return "memory";
  return {};
}

HelperRun superviseHelper(QProcess& helper, int deadlineMs) {
  QElapsedTimer timer;
  timer.start();
  helper.start();
  HelperRun run;
  if (!helper.waitForStarted(deadlineMs)) {
    run.cause = "crashed";
    return run;
  }
  while (helper.state() != QProcess::NotRunning) {
    helper.waitForFinished(20);
    run.output += helper.readAllStandardOutput();
    run.cause = breach(helper, timer, run.output.size(), deadlineMs);
    if (run.cause.isEmpty()) continue;
    helper.kill();
    helper.waitForFinished(3000);
    run.elapsedMs = timer.elapsed();
    return run;
  }
  run.output += helper.readAllStandardOutput();
  run.cause     = exitCause(helper);
  run.elapsedMs = timer.elapsed();
  return run;
}

void configureHelper(QProcess& helper, const Context& context,
                     const QString& directory, const QString& kind) {
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.remove("GROUNDZERO_AUTOMATION_CONTROL_FD");
  helper.setProcessEnvironment(environment);
  helper.setWorkingDirectory(directory);
  helper.setProcessChannelMode(QProcess::SeparateChannels);
  helper.setStandardInputFile(QProcess::nullDevice());
  helper.setStandardErrorFile(QProcess::nullDevice());
  helper.setProgram(QCoreApplication::applicationFilePath());
  QStringList arguments{"--automation-worker", "--workspace", directory,
                        "--parse-helper", kind};
#ifdef BUILD_UI_PORT_TESTS
  if (!context.childFault.isEmpty())
    arguments << "--test-child-fault" << context.childFault;
#else
  Q_UNUSED(context);
#endif
  helper.setArguments(arguments);
}

QString causeText(const QString& cause) {
  if (cause == "timeout") return "did not finish within 10 seconds";
  if (cause == "memory") return "exceeded its 1 GiB memory bound";
  return "terminated abnormally";
}

QJsonObject interpret(const HelperRun& run, const QString& failureCode) {
  if (!run.cause.isEmpty())
    throw Error(failureCode,
                "The native parser " + causeText(run.cause) +
                    "; the worker discarded its private copy.",
                {{"cause", run.cause}});
  QJsonParseError parse;
  const QJsonDocument reply = QJsonDocument::fromJson(run.output, &parse);
  const QJsonObject object  = reply.object();
  const QString status      = object.value("status").toString();
  if (parse.error != QJsonParseError::NoError || !reply.isObject())
    throw Error(failureCode, "The parse helper returned no summary.",
                {{"cause", "crashed"}});
  if (status == "parsed" && object.value("summary").isObject())
    return object.value("summary").toObject();
  const QString message = object.value("message").toString().left(256);
  if (status == "limit")
    throw Error(
        failureCode == "unsupported_palette" ? failureCode : "resource_limit",
        "The file is outside the admitted bounds: " + message,
        {{"cause", "out_of_bounds"}});
  throw Error(failureCode, "The native parser rejected the file.",
              {{"cause", "rejected"}, {"native_message", message}});
}

}  // namespace

ParseHelperRun::ParseHelperRun(Context& context, const QString& directory,
                               const QString& kind)
    : m_process(std::make_unique<QProcess>()) {
  configureHelper(*m_process, context, directory, kind);
  m_timer.start();
  m_process->start();
  m_running = true;
}

ParseHelperRun::~ParseHelperRun() { kill(); }

bool ParseHelperRun::poll() {
  if (!m_running) return false;
  const QByteArray chunk = m_process->readAllStandardOutput();
  m_read += chunk.size();
  m_output += chunk;
  if (m_process->state() == QProcess::NotRunning) {
    m_process->waitForFinished(0);
    m_output += m_process->readAllStandardOutput();
    m_cause   = m_process->error() == QProcess::FailedToStart
                    ? QString("crashed")
                    : exitCause(*m_process);
    m_running = false;
    return false;
  }
  const QString cause =
      breach(*m_process, m_timer, m_read, SharedParseDeadlineMs);
  if (cause.isEmpty()) return true;
  kill();
  m_cause = cause;
  return false;
}

QList<QByteArray> ParseHelperRun::takeLines() {
  QList<QByteArray> lines;
  for (int end = m_output.indexOf('\n'); end >= 0;
       end     = m_output.indexOf('\n')) {
    lines.append(m_output.left(end));
    m_output.remove(0, end + 1);
  }
  return lines;
}

void ParseHelperRun::kill() {
  if (!m_running) return;
  m_running = false;
  m_cause   = "crashed";
  m_process->kill();
  m_process->waitForFinished(3000);
}

QJsonObject parseSharedFile(Context& context, const QString& kind,
                            const QByteArray& bytes,
                            const QString& failureCode) {
  if (context.runtimeDirectory.isEmpty())
    throw Error("io_error", "The worker has no private runtime directory.");
  QTemporaryDir directory(
      QDir(context.runtimeDirectory).filePath("shared-parse-XXXXXX"));
  QFile copy(QDir(directory.path()).filePath("input." + extension(kind)));
  if (!directory.isValid() || !copy.open(QIODevice::WriteOnly) ||
      copy.write(bytes) != bytes.size() || !copy.flush())
    throw Error("io_error", "Cannot write the private parse copy.");
  copy.close();
  QProcess helper;
  configureHelper(helper, context, directory.path(), kind);
  return interpret(superviseHelper(helper, helperDeadlineMs(kind)),
                   failureCode);
}

HelperProbeRun runProbeHelper(Context& context, const QString& kind) {
  if (context.runtimeDirectory.isEmpty())
    throw Error("io_error", "The worker has no private runtime directory.");
  QTemporaryDir directory(
      QDir(context.runtimeDirectory).filePath("probe-XXXXXX"));
  if (!directory.isValid())
    throw Error("io_error", "Cannot create the private probe directory.");
  QProcess helper;
  configureHelper(helper, context, directory.path(), kind);
  const HelperRun run = superviseHelper(helper, helperDeadlineMs(kind));
  HelperProbeRun result;
  result.cause              = run.cause;
  result.elapsedMs          = run.elapsedMs;
  const QJsonDocument reply = QJsonDocument::fromJson(run.output);
  if (run.cause.isEmpty() && !reply.isObject()) result.cause = "crashed";
  if (reply.isObject()) result.reply = reply.object();
  return result;
}

int runSharedParseHelper(Context& context, const QString& kind, QFile& output) {
  startSelfWatchdog(helperDeadlineMs(kind));
#ifdef BUILD_UI_PORT_TESTS
  injectChildFault(context.childFault);
#endif
  if (kind == "media") return runMediaDecodeHelper(context, output);
  const QJsonObject reply =
      kind == "gl_probe" ? graphicsProbeReply() : helperReply(context, kind);
  const QByteArray line =
      QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
  if (output.write(line) != line.size() || !output.flush()) return 2;
  return 0;
}

qint64 processMemoryBytes(qint64 processId) {
#ifdef _WIN32
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
                  DWORD(processId));
  if (!process) return -1;
  PROCESS_MEMORY_COUNTERS_EX counters{};
  const BOOL read = K32GetProcessMemoryInfo(
      process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
      sizeof(counters));
  CloseHandle(process);
  return read ? qint64(counters.PrivateUsage) : -1;
#elif defined(__APPLE__)
  rusage_info_v2 usage{};
  if (proc_pid_rusage(int(processId), RUSAGE_INFO_V2,
                      reinterpret_cast<rusage_info_t*>(&usage)) != 0)
    return -1;
  return qint64(usage.ri_phys_footprint);
#else
  QFile statm(QString("/proc/%1/statm").arg(processId));
  if (!statm.open(QIODevice::ReadOnly)) return -1;
  const QList<QByteArray> fields = statm.readAll().split(' ');
  bool valid                     = false;
  const qint64 pages = fields.size() > 1 ? fields[1].toLongLong(&valid) : 0;
  return valid ? pages * sysconf(_SC_PAGESIZE) : -1;
#endif
}

}  // namespace GroundzeroAutomation
