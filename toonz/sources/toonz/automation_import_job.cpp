#include "automation_import.h"

#include "automation_build_identity.h"
#include "automation_files.h"
#include "automation_fx_profile.h"
#include "automation_jobs.h"
#include "automation_package.h"
#include "automation_scene_host.h"

#include "texception.h"
#include "tfx.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "trop.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>

#include <atomic>
#include <memory>
#include <set>

namespace GroundzeroAutomation {
namespace {

constexpr int ImportDeadlineMs = 600000;

const QStringList& lossKinds() {
  static const QStringList kinds{
      "unrecognized_scene_tag", "sub_xsheet",
      "fx_not_admitted",        "fx_value_out_of_range",
      "level_not_admitted",     "level_load_failed",
      "raster_depth_reduced",   "empty_level",
      "style_not_admitted",     "column_not_admitted",
      "exposure_not_admitted",  "sound_not_admitted",
      "camera_not_active",      "motion_path_not_admitted",
      "rig_not_admitted",       "storyboard_not_admitted",
      "cast_organization",      "library_origin"};
  return kinds;
}

// ------------------------------------------------------------ conversion

// Removes an effect node as native deletion does: what it fed now reads its
// first input, which also takes its place among the terminal effects.
void removeBridged(FxDag* dag, TFx* fx) {
  TFx* input =
      fx->getInputPortCount() > 0 ? fx->getInputPort(0)->getFx() : nullptr;
  while (fx->getOutputConnectionCount() > 0)
    fx->getOutputConnection(0)->setFx(input);
  const bool terminal = dag->getTerminalFxs()->containsFx(fx);
  fx->disconnectAll();
  dag->removeFxId(fx);
  dag->removeFromXsheet(fx);
  dag->getInternalFxs()->removeFx(fx);
  if (terminal && input) dag->addToXsheet(input);
}

void clearCellsWhere(TXsheet* sheet,
                     const std::function<bool(const TXshCell&)>& match) {
  for (int column = 0; column < sheet->getColumnCount(); ++column) {
    TXshColumn* native = sheet->getColumn(column);
    if (!native || native->isEmpty() || !native->getCellColumn()) continue;
    int first = 0, last = -1;
    native->getRange(first, last);
    for (int row = std::max(first, 0); row <= last; ++row)
      if (match(sheet->getCell(row, column))) sheet->clearCells(row, column);
  }
}

// 16-bit and float drawings become 8-bit RGBA with the same DPI.
void reduceDepth(TXshSimpleLevel* level) {
  for (const TFrameId& frame : level->getFids()) {
    const TRasterImageP image = level->getFrame(frame, false);
    if (!image || TRaster32P(image->getRaster())) continue;
    const TRasterP source = image->getRaster();
    TRaster32P converted(source->getLx(), source->getLy());
    TRop::convert(converted, source);
    TRasterImageP result(new TRasterImage(converted));
    double x = 0, y = 0;
    image->getDpi(x, y);
    result->setDpi(x, y);
    level->setFrame(frame, result);
  }
  level->set16BitChannelLevel(false);
  level->setFloatChannelLevel(false);
  level->getProperties()->setBpp(32);
}

const std::vector<ImportItem>& itemsOf(const ImportPlan& plan,
                                       const QString& kind) {
  static const std::vector<ImportItem> none;
  const auto found = plan.losses.find(kind);
  return found == plan.losses.end() ? none : found->second.items;
}

void convertEffects(ToonzScene& scene, const ImportPlan& plan,
                    std::set<TXshColumn*>& columns) {
  FxDag* dag = scene.getXsheet()->getFxDag();
  for (const ImportItem& item : itemsOf(plan, "fx_value_out_of_range")) {
    auto* wrapper = dynamic_cast<TZeraryColumnFx*>(item.fx);
    clampAutomationFxValues(wrapper ? wrapper->getZeraryFx() : item.fx);
  }
  for (const ImportItem& item : itemsOf(plan, "fx_not_admitted")) {
    if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(item.fx)) {
      dag->removeFxId(wrapper->getZeraryFx());
      columns.insert(wrapper->getXshColumn());
    } else {
      removeBridged(dag, item.fx);
    }
  }
}

void convertLevels(ToonzScene& scene, const ImportPlan& plan) {
  std::set<TXshLevel*> styled;
  for (const ImportItem& item : itemsOf(plan, "style_not_admitted"))
    if (styled.insert(item.level).second)
      admitImportStyles(item.level->getSimpleLevel()->getPalette());
  for (const ImportItem& item : itemsOf(plan, "library_origin")) {
    item.level->setLibraryProvenance(TXshLevel::LibraryProvenance());
    TXshSimpleLevel* simple = item.level->getSimpleLevel();
    if (simple && simple->getPalette())
      simple->getPalette()->setGlobalName(L"");
  }
  for (const ImportItem& item : itemsOf(plan, "raster_depth_reduced"))
    reduceDepth(item.level->getSimpleLevel());
  TXsheet* sheet = scene.getXsheet();
  for (const ImportItem& item : itemsOf(plan, "exposure_not_admitted"))
    clearCellsWhere(sheet, [&item](const TXshCell& cell) {
      return cell.m_level.getPointer() == item.level &&
             cell.m_frameId == item.frame;
    });
  std::set<TXshLevel*> dropped;
  for (const char* kind :
       {"sub_xsheet", "level_not_admitted", "level_load_failed", "empty_level"})
    for (const ImportItem& item : itemsOf(plan, kind))
      dropped.insert(item.level);
  clearCellsWhere(sheet, [&dropped](const TXshCell& cell) {
    return dropped.count(cell.m_level.getPointer()) > 0;
  });
  for (TXshLevel* level : dropped) scene.getLevelSet()->removeLevel(level);
}

// Applies every loss of the snapshot's own plan; returns the applied counts.
QJsonArray applyLosses(ToonzScene& scene, const ImportPlan& plan) {
  std::set<TXshColumn*> columns;
  for (const ImportItem& item : itemsOf(plan, "column_not_admitted"))
    columns.insert(item.column);
  convertEffects(scene, plan, columns);
  convertLevels(scene, plan);
  TXsheet* sheet = scene.getXsheet();
  for (int index = sheet->getColumnCount() - 1; index >= 0; --index)
    if (columns.count(sheet->getColumn(index))) sheet->removeColumn(index);
  sheet->updateFrameCount();
  QJsonArray applied;
  for (const auto& entry : plan.losses)
    applied.append(QJsonObject{{"kind", entry.first},
                               {"count", double(entry.second.items.size())}});
  return applied;
}

// ------------------------------------------------------------ provenance

QString hexDigest(const QString& text) {
  return QString::fromLatin1(
      QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}

// The read root holding the scene: the workspace, else the input root, with
// the scene's path below it as stored. No absolute path is recorded.
QJsonObject sourceObject(Document& document) {
  const ReadOnlyScene& source = *document.readOnly;
  QString root;
  for (const QString& candidate : source.readRoots)
    if (source.scenePath.startsWith(candidate + '/') &&
        (root.isEmpty() || candidate.size() < root.size()))
      root = candidate;
  QJsonArray tags;
  for (const std::string& tag : document.scene->getUnrecognizedSceneTags())
    if (tags.size() < 32) tags.append(QString::fromStdString(tag).left(128));
  const auto version = document.scene->getVersionNumber();
  return {{"root", root == source.workerWorkspace ? "workspace" : "input_root"},
          {"root_path_sha256", hexDigest(root)},
          {"relative_path", source.scenePath.mid(root.size() + 1)},
          {"scene_file_revision", source.sceneRevision},
          {"source_fingerprint", source.fingerprint},
          {"project_id", source.projectId},
          {"scene_version",
           QString("%1.%2").arg(version.first).arg(version.second)},
          {"unrecognized_scene_tags", tags}};
}

QJsonObject provenanceObject(Document& document, const QString& planDigest,
                             const QJsonArray& applied) {
  return {{"kind", "import"},
          {"source", sourceObject(document)},
          {"importer",
           QJsonObject{{"profile", "worker-generated-v1"},
                       {"classifier_version", ImportClassifierVersion},
                       {"executable_sha256",
                        buildIdentity().value("build_digest").toString()}}},
          {"plan_digest", planDigest},
          {"losses_applied", applied},
          {"source_copy", packageSourceScenePath()}};
}

// ------------------------------------------------------------------- job

enum class State { Running, Completed, Failed, Canceled, DeadlineExceeded };
enum class Phase { Snapshot, Convert, Write, Publish };

QString stateText(State state) {
  switch (state) {
  case State::Running:
    return "running";
  case State::Completed:
    return "completed";
  case State::Failed:
    return "failed";
  case State::Canceled:
    return "canceled";
  case State::DeadlineExceeded:
    return "deadline_exceeded";
  }
  return "failed";
}

// Thrown from the writer's progress callback when a cancellation arrives.
struct Interrupted final {};

// One scene_import job in a scene host: a fresh snapshot of the private scene
// copy is classified again, converted, written and verified in private
// storage, and published under <host>/imports/<job_id>/package only after the
// pinned sources are re-checked. A failed or canceled job leaves nothing.
class ImportJob final : public Job,
                        public std::enable_shared_from_this<ImportJob> {
public:
  ImportJob(Context& context, Document& document, QString planDigest,
            QByteArray shape)
      : m_context(context)
      , m_planDigest(std::move(planDigest))
      , m_shape(std::move(shape))
      , m_pins(document.readOnly->pins) {
    JobRegistry& registry    = jobRegistry(context);
    common.id                = registry.newJobId();
    common.kind              = "scene_import";
    common.lane              = JobLane::Render;
    common.documentId        = document.id;
    common.revision          = document.revisionText();
    common.submittedMs       = registry.elapsedMs();
    common.sourceFingerprint = document.readOnly->fingerprint;
  }

  JobCommon common;

  QString id() const override { return common.id; }
  JobLane lane() const override { return JobLane::Render; }
  QString documentId() const override { return common.documentId; }
  bool terminal() const override { return m_state != State::Running; }
  qint64 reservedBytes() const override { return 0; }

  void start() {
    common.startedMs = now();
    m_clock.start();
    schedule();
  }

  void cancel() override {
    if (!common.cancelRequested) {
      const qint64 asked     = m_interruptMs.load();
      common.cancelRequested = true;
      common.cancelMs        = asked >= 0 ? asked : now();
    }
    if (!terminal()) end(State::Canceled);
  }

  void interrupt(qint64 requestedMs) override {
    qint64 unset = -1;
    m_interruptMs.compare_exchange_strong(unset, requestedMs);
    m_interrupted = true;
  }

  QJsonObject record() const override {
    const QJsonObject limits{{"deadline_ms", ImportDeadlineMs},
                             {"storage_reserved_bytes", 0},
                             {"cancel_grace_ms", CancelGraceMs}};
    const QJsonObject progress{{"unit", "drawings"},
                               {"done", double(m_done)},
                               {"total", double(m_total)}};
    const bool done = m_state == State::Completed;
    const QJsonObject outputs{
        {"directory", QJsonValue()}, {"artifact_count", 0}, {"result_rows", 0}};
    QJsonObject result = commonRecord(m_context, common, stateText(m_state),
                                      terminal(), limits, progress, outputs);
    const auto when    = [done](const QJsonValue& value) {
      return done ? value : QJsonValue();
    };
    result.insert(
        "import",
        QJsonObject{{"plan_digest", m_planDigest},
                    {"package_id", when("package:" + m_manifestSha256)},
                    {"package_bytes", when(double(m_packageBytes))},
                    {"levels", when(m_levels)},
                    {"drawings", when(m_drawings)},
                    {"losses_applied", m_applied},
                    {"source_copy", true},
                    {"adopted_documents", 0}});
    return result;
  }

  QJsonValue result(Context&, const QJsonObject&) override {
    throw Error("result_unavailable",
                "An import job has no result pages; open its package with "
                "scene.open {import_job_id} once it completed.",
                {{"status", stateText(m_state)}, {"next", "scene.open"}});
  }

private:
  qint64 now() const { return jobRegistry(m_context).elapsedMs(); }

  void schedule() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self]() { self->step(); });
  }

  void step() {
    runPhase();
    jobRegistry(m_context).publish();
  }

  QString importDirectory() const {
    return QDir(m_context.workspace()).filePath("imports/" + common.id);
  }

  void runPhase() {
    if (terminal()) return;
    if (m_interrupted) return end(State::Canceled);
    if (m_clock.elapsed() > ImportDeadlineMs)
      return end(State::DeadlineExceeded);
    try {
      switch (m_phase) {
      case Phase::Snapshot:
        snapshot();
        break;
      case Phase::Convert:
        convert();
        break;
      case Phase::Write:
        write();
        break;
      case Phase::Publish:
        publish();
        return;
      }
    } catch (const Interrupted&) {
      return end(State::Canceled);
    } catch (const Error& error) {
      return fail(error.code, error.message);
    } catch (const TException& error) {
      return fail("native_error", QString::fromStdWString(error.getMessage()));
    } catch (const std::exception& error) {
      return fail("native_error", QString::fromUtf8(error.what()));
    }
    if (!terminal()) schedule();
  }

  Document& sourceDocument() { return m_context.document(common.documentId); }

  // The snapshot must classify exactly as the plan the caller consented to.
  void snapshot() {
    m_snapshot = loadImportSnapshot(m_context, sourceDocument());
    m_plan     = classifyImport(*m_snapshot);
    if (importPlanShape(m_plan) != m_shape)
      throw Error("import_verification_failed",
                  "The snapshot of the source classifies differently from "
                  "the plan; nothing was imported.");
    m_total = 2 * m_plan.estimate.value("drawings").toInt();
    m_phase = Phase::Convert;
  }

  void convert() {
    m_applied = applyLosses(*m_snapshot->scene, m_plan);
    try {
      validateAutomationScene(*m_snapshot->scene);
    } catch (const Error& error) {
      throw Error("import_verification_failed",
                  "The converted scene is outside the editable profile (" +
                      error.code + "): " + error.message);
    }
    m_phase = Phase::Write;
  }

  void write() {
    m_staging = std::make_unique<PrivateDirectory>(
        m_context.scratchDirectory() + "/import-XXXXXX");
    if (!m_staging->isValid())
      throw Error("package_write_failed",
                  "Cannot create the private import staging directory.");
    PackageExtras extras;
    extras.provenance =
        provenanceObject(sourceDocument(), m_planDigest, m_applied);
    extras.sourceScene = sourceDocument().readOnly->sceneCopy;
    extras.progress    = [this](int done) {
      m_done = done;
      jobRegistry(m_context).publish();
      if (m_interrupted) throw Interrupted();
    };
    const VerifiedPackage package = writeVerifiedPackage(
        m_context, *m_snapshot->scene, "0", m_staging->path(), extras);
    m_manifestSha256 = package.manifestSha256;
    m_packageBytes   = package.bytes;
    m_drawings       = package.drawings;
    m_levels         = package.manifest.value("levels").toArray().size();
    m_done           = m_total;
    m_phase          = Phase::Publish;
  }

  // Publishes only when no pinned source changed during the conversion. The
  // test fault point import:written (hold or crash) stops here first.
  void publish() {
    writeFaultFires(m_context, "import:written");
    if (m_interrupted) return end(State::Canceled);
    const QStringList changed = changedSourcePins(m_pins);
    if (!changed.isEmpty())
      return fail("source_changed",
                  QString("%1 pinned source file(s) changed during the "
                          "import, including %2")
                      .arg(changed.size())
                      .arg(changed.first()));
    const QString directory = importDirectory();
    if (!QDir().mkpath(directory) ||
        !QDir().rename(m_staging->path(), directory + "/package"))
      return fail("package_write_failed",
                  "Cannot place the verified import package.");
    m_staging->setAutoRemove(false);
    m_staging.reset();
    end(State::Completed);
  }

  void fail(const QString& code, const QString& message) {
    common.failure = QJsonObject{
        {"code", code}, {"phase", "executor"}, {"message", message.left(2048)}};
    end(State::Failed);
  }

  // A job that did not complete leaves no import directory and no staging.
  void end(State state) {
    m_state        = state;
    common.endedMs = now();
    m_snapshot.reset();
    m_staging.reset();
    if (state != State::Completed && QFileInfo::exists(importDirectory()))
      removeApprovedTree(importDirectory());
  }

  Context& m_context;
  QString m_planDigest;
  QByteArray m_shape;
  std::vector<SourcePin> m_pins;
  QElapsedTimer m_clock;
  State m_state = State::Running;
  Phase m_phase = Phase::Snapshot;
  std::unique_ptr<Document> m_snapshot;
  ImportPlan m_plan;
  std::unique_ptr<PrivateDirectory> m_staging;
  QJsonArray m_applied;
  QString m_manifestSha256;
  qint64 m_packageBytes = 0;
  int m_levels          = 0;
  int m_drawings        = 0;
  int m_done            = 0;
  int m_total           = 0;
  std::atomic<qint64> m_interruptMs{-1};
  std::atomic<bool> m_interrupted{false};
};

QStringList acceptedKinds(const QJsonObject& params) {
  QStringList accepted;
  for (const QJsonValue& value : params.value("accept_losses").toArray()) {
    const QString kind = value.toString();
    if (!lossKinds().contains(kind) || accepted.contains(kind))
      throw Error("invalid_params", "accept_losses holds unique loss kinds.",
                  {{"kind", kind}});
    accepted.append(kind);
  }
  return accepted;
}

void requireRenderLane(Context& context) {
  const QString active = activeRenderJob(context);
  if (!active.isEmpty())
    throw Error("render_busy",
                "Wait for the active render or import job to finish or "
                "cancel it.",
                {{"job_id", active}});
  if (renderJobCount(context) >= JobsPerLane)
    throw Error("render_limit",
                "This worker has reached its render job limit.");
}

QJsonObject admittedRequest(const QJsonObject& params) {
  QJsonObject request = params;
  request.remove("operation_id");
  request.remove("expected_revision");
  return request;
}

}  // namespace

QJsonValue startImportJob(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (!document.readOnly)
    throw Error("unsupported_target",
                "scene.import_editable imports a document opened with "
                "scene.open_read_only; this one is already editable.",
                {{"reason", "editable_document"}});
  const QStringList accepted = acceptedKinds(params);
  requireRenderLane(context);
  requireUnchangedSources(document.readOnly->pins);
  const ImportPlan plan = classifyImport(document);
  const QString digest = importPlanDigest(plan, document.readOnly->fingerprint);
  if (digest != requiredString(params, "plan_digest"))
    throw Error("plan_changed",
                "The import plan changed; call scene.import_plan again and "
                "quote its plan_digest.",
                {{"plan_digest", digest}});
  if (!plan.importable())
    throw Error("import_blocked",
                "The plan has blockers; this scene cannot be imported.",
                {{"blockers", importBlockers(plan, 16)}});
  QJsonArray missing;
  for (const auto& entry : plan.losses)
    if (!accepted.contains(entry.first)) missing.append(entry.first);
  if (!missing.isEmpty())
    throw Error("losses_not_accepted",
                "Accept every loss kind of the plan in accept_losses.",
                {{"missing", missing}});
  auto job            = std::make_shared<ImportJob>(context, document, digest,
                                                    importPlanShape(plan));
  job->common.request = admittedRequest(params);
  context.markMutationStarted();
  jobRegistry(context).add(job);
  job->start();
  return job->record();
}

}  // namespace GroundzeroAutomation
