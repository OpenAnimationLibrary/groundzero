#include "automation_package.h"

#include "automation_audio.h"
#include "automation_drawing.h"
#include "automation_files.h"
#include "automation_journal.h"
#include "automation_production.h"

#include "texception.h"
#include "toonz/fxdag.h"
#include "toonz/hook.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshsoundlevel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>

#ifdef _WIN32
#include <QStorageInfo>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
#include <cerrno>
#include <set>
#endif

#include <algorithm>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 SpaceMarginBytes = 64 * 1024 * 1024;
// P-PS-6: allowances over the native drawing estimate, so the bound is never
// below the bytes a save writes.
constexpr qint64 FileOverheadBytes  = 4096;
constexpr qint64 HookFrameBytes     = 256;
constexpr qint64 SceneBaseBytes     = 256 * 1024;
constexpr qint64 SceneCellBytes     = 64;
constexpr qint64 SceneEntityBytes   = 8192;
constexpr qint64 ManifestBaseBytes  = 64 * 1024;
constexpr qint64 ManifestEntryBytes = 512;

const QStringList& checkNames() {
  static const QStringList names{
      "document",   "root",  "path", "parent",  "collision",
      "filesystem", "space", "size", "journal", "external_paths"};
  return names;
}

// The ten checks in their reporting order; one not set is unknown.
class Checks final {
public:
  void pass(const QString& name) { set(name, "pass", {}, {}); }
  void fail(const QString& name, const QString& code, const QString& detail) {
    set(name, "fail", code, detail);
  }
  void unknown(const QString& name, const QString& detail) {
    set(name, "unknown", {}, detail);
  }

  QJsonArray report() const {
    QJsonArray result;
    for (const QString& name : checkNames())
      result.append(
          m_checks.value(name, QJsonObject{{"name", name},
                                           {"state", "unknown"},
                                           {"code", QJsonValue()},
                                           {"detail", "Not checked."}}));
    return result;
  }

  QJsonValue firstFailure() const {
    for (const QString& name : checkNames())
      if (m_checks.value(name).value("state") == "fail") return name;
    return QJsonValue();
  }

private:
  void set(const QString& name, const QString& state, const QString& code,
           const QString& detail) {
    m_checks.insert(
        name, QJsonObject{{"name", name},
                          {"state", state},
                          {"code", code.isEmpty() ? QJsonValue() : code},
                          {"detail", detail.isEmpty()
                                         ? QJsonValue()
                                         : QJsonValue(detail.left(1024))}});
  }

  QMap<QString, QJsonObject> m_checks;
};

// What the file system says about the destination at this moment.
struct Facts final {
  QString rootCode;
  QString rootDetail;
  QString pathCode;
  QString pathDetail;
  bool parentFound = false;
  bool writable    = false;
  int exists       = -1;  // 1, 0, or -1 when it cannot be told
  qint64 freeBytes = -1;
  int sameVolume   = -1;  // 1, 0, or -1 when it cannot be told
  QString rename   = "unknown";
};

#ifdef _WIN32

void describeParent(int, const QString&, const Context&, Facts&) {}

Facts observeWorkspace(const Context& context, const PackageTarget& target) {
  Facts facts;
  const QFileInfo parent(QFileInfo(target.absolute).absolutePath());
  facts.parentFound = parent.isDir() && !parent.isSymLink();
  if (!facts.parentFound) return facts;
  facts.writable = parent.isWritable();
  const QFileInfo entry(target.absolute);
  facts.exists = entry.exists() || entry.isSymLink() ? 1 : 0;
  const QStorageInfo volume(parent.absoluteFilePath());
  facts.freeBytes = volume.bytesAvailable();
  facts.sameVolume =
      volume.rootPath() == QStorageInfo(context.workspace()).rootPath();
  return facts;
}

#else

QString renameSupport(int parent, qint64 volume) {
  if (exclusivePublicationRefused(volume)) return "failed_last_attempt";
  struct statfs info{};
  if (::fstatfs(parent, &info) != 0) return "unknown";
#ifdef __APPLE__
  const QByteArray type(info.f_fstypename);
  return type == "apfs" || type == "hfs" ? "expected" : "unknown";
#else
  // ext4, XFS, Btrfs, tmpfs and F2FS implement RENAME_NOREPLACE.
  static const std::set<unsigned long> types{
      0xEF53UL, 0x58465342UL, 0x9123683EUL, 0x01021994UL, 0xF2F52010UL};
  return types.count((unsigned long)info.f_type) ? "expected" : "unknown";
#endif
}

void describeParent(int parent, const QString& name, const Context& context,
                    Facts& facts) {
  facts.parentFound = true;
  facts.writable    = ::faccessat(parent, ".", W_OK, 0) == 0;
  struct stat entry{};
  if (::fstatat(parent, QFile::encodeName(name).constData(), &entry,
                AT_SYMLINK_NOFOLLOW) == 0)
    facts.exists = 1;
  else if (errno == ENOENT)
    facts.exists = 0;
  struct statvfs space{};
  if (::fstatvfs(parent, &space) == 0)
    facts.freeBytes = qint64(space.f_bavail) * qint64(space.f_frsize);
  struct stat here{};
  struct stat home{};
  if (::fstat(parent, &here) != 0) return;
  if (::stat(QFile::encodeName(context.workspace()).constData(), &home) == 0)
    facts.sameVolume = here.st_dev == home.st_dev ? 1 : 0;
  facts.rename = renameSupport(parent, qint64(here.st_dev));
}

Facts observeWorkspace(const Context& context, const PackageTarget& target) {
  Facts facts;
  const QString parent = QFileInfo(target.absolute).absolutePath();
  const int directory = ::open(QFile::encodeName(parent).constData(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) {
    if (errno == ELOOP) {
      facts.pathCode   = "path_denied";
      facts.pathDetail = "The package parent directory is a link.";
    }
    return facts;
  }
  describeParent(directory, target.name, context, facts);
  ::close(directory);
  return facts;
}

#endif

Facts observeOutputRoot(const Context& context, const PackageTarget& target) {
  Facts facts;
  int directory = -1;
  try {
    directory = openRootDirectory(target.rootPath, QString());
  } catch (const Error& error) {
    facts.rootCode   = error.code;
    facts.rootDetail = error.message;
    return facts;
  }
  ::close(directory);
  try {
    directory = openRootDirectory(target.rootPath, target.parent);
  } catch (const Error& error) {
    facts.pathCode   = error.code;
    facts.pathDetail = error.message;
    return facts;
  }
  if (directory < 0) return facts;
  describeParent(directory, target.name, context, facts);
  ::close(directory);
  return facts;
}

// An upper bound of the package: the copied files' known sizes and a bound
// of everything encoded from memory.
struct Estimate final {
  qint64 known   = 0;
  qint64 encoded = 0;
  qint64 files   = 2;  // scene.tnz and manifest.json
  QString code;
  QString detail;
};

int levelFiles(TXshSimpleLevel& level) {
  if (level.getType() == PLI_XSHLEVEL) return 1;
  if (level.getType() == TZP_XSHLEVEL) return 2;  // .tlv and .tpl
  return level.getFrameCount();                   // one PNG per drawing
}

void estimateLevel(TXshSimpleLevel& level, Estimate& estimate) {
  const qint64 files    = levelFiles(level);
  const qint64 drawings = level.getFrameCount();
  estimate.files += files;
  estimate.encoded += automationLevelBytes(level) + files * FileOverheadBytes +
                      drawings * ManifestEntryBytes;
  const qint64 hooks = level.getHookSet()->getHookCount();
  if (hooks == 0) return;
  estimate.files += 1;
  estimate.encoded +=
      FileOverheadBytes + hooks * (drawings + 1) * HookFrameBytes;
}

Estimate estimate(ToonzScene& scene) {
  Estimate result;
  try {
    TLevelSet* levels = scene.getLevelSet();
    for (int index = 0; index < levels->getLevelCount(); ++index) {
      TXshLevel* level = levels->getLevel(index);
      if (TXshSoundLevel* sound = level->getSoundLevel()) {
        result.known += std::max<qint64>(
            QFileInfo(sound->getPath().getQString()).size(), 0);
        result.files += 1;
      } else if (TXshSimpleLevel* simple = level->getSimpleLevel()) {
        estimateLevel(*simple, result);
      }
    }
    TXsheet* sheet        = scene.getXsheet();
    const qint64 columns  = sheet->getColumnCount();
    const qint64 entities = columns + levels->getLevelCount() +
                            sheet->getFxDag()->getInternalFxs()->getFxCount();
    result.encoded += SceneBaseBytes + ManifestBaseBytes +
                      columns * sheet->getFrameCount() * SceneCellBytes +
                      entities * SceneEntityBytes +
                      result.files * ManifestEntryBytes;
  } catch (const Error& error) {
    result.code   = error.code;
    result.detail = error.message;
  }
  return result;
}

// One dependency row of the walk, mapped to where the save puts it.
class ItemMapper final {
public:
  explicit ItemMapper(Document& document) {
    TLevelSet* levels = document.scene->getLevelSet();
    for (int index = 0; index < levels->getLevelCount(); ++index) {
      TXshLevel* level = levels->getLevel(index);
      m_levels.insert(document.idForLevel(level), {index, level});
    }
  }

  QJsonObject item(const QJsonObject& row) const {
    const QString role = row.value("role").toString();
    const Slot slot    = row.value("owner_kind") == "level"
                             ? m_levels.value(row.value("owner_id").toString())
                             : Slot{};
    const QString path = slot.level ? packagePath(role, slot) : QString();
    const QString source = path.isEmpty()
                               ? "not_packaged"
                               : (role == "sound" ? "private_copy" : "memory");
    const QString found  = row.value("availability").toString();
    return {
        {"role", role},
        {"owner_kind", row.value("owner_kind")},
        {"owner_id", row.value("owner_id")},
        {"name",
         slot.level ? QJsonValue(QString::fromStdWString(slot.level->getName()))
                    : QJsonValue()},
        {"coded_path", row.value("coded_path").toString().isEmpty()
                           ? QJsonValue()
                           : row.value("coded_path")},
        {"package_path", path.isEmpty() ? QJsonValue() : path},
        {"source", source},
        {"availability", availability(found, source)},
        {"size_bytes",
         source == "memory" ? QJsonValue() : row.value("size_bytes")}};
  }

  // A row for each level whose hooks are held only in memory: the save
  // writes their sidecar though no file is there yet.
  QJsonArray unsavedHooks(const QJsonArray& rows) const {
    QSet<QString> listed;
    for (const QJsonValue& row : rows)
      if (row.toObject().value("role") == "hook_sidecar")
        listed.insert(row.toObject().value("owner_id").toString());
    QJsonArray result;
    for (auto it = m_levels.constBegin(); it != m_levels.constEnd(); ++it) {
      TXshSimpleLevel* simple = it.value().level->getSimpleLevel();
      if (!simple || listed.contains(it.key()) ||
          simple->getHookSet()->getHookCount() == 0)
        continue;
      result.append(item({{"role", "hook_sidecar"},
                          {"owner_kind", "level"},
                          {"owner_id", it.key()},
                          {"coded_path", QJsonValue()},
                          {"availability", "unsaved"},
                          {"size_bytes", QJsonValue()}}));
    }
    return result;
  }

private:
  struct Slot final {
    int index        = -1;
    TXshLevel* level = nullptr;
  };

  static QString packagePath(const QString& role, const Slot& slot) {
    if (TXshSoundLevel* sound = slot.level->getSoundLevel())
      return role == "sound" ? automationSoundPackagePath(slot.index, *sound)
                             : QString();
    const int type = slot.level->getType();
    if (type != PLI_XSHLEVEL && type != TZP_XSHLEVEL && type != OVL_XSHLEVEL)
      return {};
    const QString level = packageLevelPath(slot.index, type);
    if (role == "primary") return level;
    if (role == "palette" && type == TZP_XSHLEVEL)
      return level.left(level.size() - 3) + "tpl";
    if (role == "hook_sidecar")
      return QDir::fromNativeSeparators(
          TXshSimpleLevel::getHookPath(TFilePath(level.toStdWString()))
              .getQString());
    return {};
  }

  static QString availability(const QString& found, const QString& source) {
    if (found == "present") return "present";
    if (found == "unsupported") return "unsupported";
    if (source == "memory" || found == "unsaved") return "unsaved";
    return "missing";
  }

  QHash<QString, Slot> m_levels;
};

// The dependency rows, mapped, and whether the walk saw all of them.
struct Items final {
  QJsonArray rows;
  bool complete = true;
};

Items dependencyItems(Context& context, Document& document) {
  DependencyWalk walk(context, document, PackageMaxDependencies);
  while (walk.step()) {
  }
  const ItemMapper mapper(document);
  Items result;
  for (const QJsonValue& row : walk.rows())
    result.rows.append(mapper.item(row.toObject()));
  for (const QJsonValue& row : mapper.unsavedHooks(walk.rows()))
    if (result.rows.size() < PackageMaxDependencies) result.rows.append(row);
  for (const QString& reason : walk.reasons())
    if (reason == "record_limit" || reason == "level_limit" ||
        reason == "fx_limit")
      result.complete = false;
  return result;
}

void checkDocument(Checks& checks, Document& document) {
  try {
    validateAutomationScene(*document.scene);
    checks.pass("document");
  } catch (const Error& error) {
    checks.fail("document", error.code, error.message);
  } catch (const TException& error) {
    checks.fail("document", "native_error",
                QString::fromStdWString(error.getMessage()));
  }
}

void checkPlace(Checks& checks, const PackageTarget& target,
                const Facts& facts) {
  if (!facts.rootCode.isEmpty()) {
    checks.fail("root", facts.rootCode, facts.rootDetail);
    return;
  }
  checks.pass("root");
  if (!facts.pathCode.isEmpty()) {
    checks.fail("path", facts.pathCode, facts.pathDetail);
    return;
  }
  checks.pass("path");
  if (!facts.parentFound) {
    checks.fail("parent", "missing_parent",
                "Create the package parent directory first.");
    return;
  }
  if (facts.writable)
    checks.pass("parent");
  else
    checks.fail("parent", "package_write_failed",
                "The package parent directory is not writable.");
  if (facts.exists == 1)
    checks.fail("collision", "already_exists",
                "Choose a new package directory; existing paths are not "
                "overwritten.");
  else if (facts.exists == 0)
    checks.pass("collision");
  else
    checks.unknown("collision", "The destination cannot be looked up.");
  if (target.root == "workspace" && facts.sameVolume == 0)
    checks.fail("filesystem", "package_write_failed",
                "The destination is on another volume than the workspace, "
                "where the package is staged.");
  else if (facts.rename == "failed_last_attempt")
    checks.fail("filesystem", "publication_unsupported",
                "A publication on this volume was refused: it cannot create "
                "a name without replacing one.");
  else if (facts.rename == "expected")
    checks.pass("filesystem");
  else
    checks.unknown("filesystem",
                   "Whether this file system renames without replacing is "
                   "not known until a save tries it.");
}

void checkSize(Checks& checks, const Estimate& bound, const Facts& facts) {
  if (!bound.code.isEmpty()) {
    checks.fail("size", bound.code, bound.detail);
    checks.unknown("space", "The package size cannot be estimated.");
    return;
  }
  const qint64 upper = bound.known + bound.encoded;
  if (bound.files > PackageMaxFiles)
    checks.fail("size", "package_limit",
                "The package would hold more than 8,192 files.");
  else if (bound.known > PackageMaxBytes)
    checks.fail("size", "package_limit", "The package exceeds 256 MiB.");
  else if (upper > PackageMaxBytes)
    checks.unknown("size",
                   "The upper bound exceeds 256 MiB; the package may still "
                   "fit.");
  else
    checks.pass("size");
  if (!facts.parentFound || facts.freeBytes < 0)
    checks.unknown("space", "The free space of the destination is unknown.");
  else if (facts.freeBytes < upper + SpaceMarginBytes)
    checks.fail("space", "insufficient_space",
                QString("%1 bytes are free; the package needs up to %2 bytes "
                        "and a 64 MiB margin.")
                    .arg(facts.freeBytes)
                    .arg(upper));
  else
    checks.pass("space");
}

void checkJournal(Checks& checks, Context& context) {
  Journal* journal = workspaceJournal(context);
  if (!journal)
    checks.fail("journal", "journal_unavailable",
                "This worker has no workspace journal; the package would not "
                "be registered.");
  else if (!journal->available())
    checks.fail("journal", "journal_unavailable", journal->unavailableReason());
  else
    checks.pass("journal");
}

void checkExternalPaths(Checks& checks, const Items& items) {
  for (const QJsonValue& value : items.rows) {
    const QJsonObject row = value.toObject();
    if (row.value("source") != "not_packaged" ||
        row.value("coded_path").isNull())
      continue;
    checks.fail("external_paths", "external_path",
                QString("The %1 of %2 %3 keeps a path outside the package.")
                    .arg(row.value("role").toString(),
                         row.value("owner_kind").toString(),
                         row.value("owner_id").toString()));
    return;
  }
  if (items.complete)
    checks.pass("external_paths");
  else
    checks.unknown("external_paths",
                   "The dependency walk stopped before its end.");
}

QJsonObject plan(Context& context, const PackageTarget& target,
                 const Estimate& bound, const Facts& facts) {
  const QJsonObject location =
      target.root == "workspace"
          ? QJsonObject{{"root", "workspace"},
                        {"path", target.relative},
                        {"input_index", QJsonValue()},
                        {"root_sha256", QJsonValue()}}
          : outputLocation(context, target.root, target.relative);
  return {
      {"location", location},
      {"staging",
       target.root == "workspace" ? "private_rename" : "private_then_copy"},
      {"save_mode", "self_contained"},
      {"files_estimate",
       double(std::min<qint64>(bound.files, PackageMaxFiles))},
      {"bytes_estimate",
       QJsonObject{{"upper_bound", double(bound.known + bound.encoded)},
                   {"known_bytes", double(bound.known)},
                   {"encoded_bytes_bound", double(bound.encoded)}}},
      {"free_bytes", facts.freeBytes < 0 ? QJsonValue()
                                         : QJsonValue(double(facts.freeBytes))},
      {"same_volume_as_workspace", facts.sameVolume < 0
                                       ? QJsonValue()
                                       : QJsonValue(facts.sameVolume == 1)}};
}

// A read-only document never reaches this handler: the dispatcher refuses it
// as unsupported_target (refuseReadOnly).
Document& editableDocument(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "stale_revision", "Refresh scene.summary before planning.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  return document;
}

}  // namespace

QJsonArray packageNotContained() {
  static const QList<QPair<QString, QString>> rows{
      {"reference_images", "not_admitted_in_editable_profile"},
      {"cast_collections", "conversion_production_c1"},
      {"fx_external_resources", "not_admitted"},
      {"models", "never_packaged"},
      {"fonts", "not_used_by_worker_scenes"},
      {"plugins", "builtin_only"},
      {"project_settings", "private_worker_project"},
      {"editor_view_state", "user_state"}};
  QJsonArray result;
  for (const auto& row : rows)
    result.append(QJsonObject{{"class", row.first}, {"reason", row.second}});
  return result;
}

QJsonObject savePreflightOperation() {
  const auto text = [](int maximum) {
    return QJsonObject{
        {"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
  };
  QJsonObject root = text(32);
  root.insert("pattern", "^[a-z][a-z0-9_-]{0,31}$");
  return operation(
      "scene.save_preflight",
      "Plan saving an editable document as a package at a destination: "
      "every dependency and where it goes in the package, every check "
      "scene.save_copy runs (collision, parent, file system, space, size, "
      "journal, external paths), an upper bound of the size and what the "
      "package will not contain. Writes nothing; a failing check is a result "
      "(ready false), not an error.",
      {{"document_id", text(128)},
       {"expected_revision", text(80)},
       {"directory", text(4096)},
       {"root", root},
       {"offset", QJsonObject{{"type", "integer"},
                              {"minimum", 0},
                              {"maximum", PackageMaxDependencies},
                              {"default", 0}}},
       {"limit", QJsonObject{{"type", "integer"},
                             {"minimum", 1},
                             {"maximum", 128},
                             {"default", 64}}}},
      {"document_id", "expected_revision", "directory"});
}

QJsonObject savePreflight(Context& context, const QJsonObject& params) {
  Document& document = editableDocument(context, params);
  const PackageTarget target =
      packageTarget(context, params, "directory", true);
  const int offset =
      integerParam(params, "offset", 0, PackageMaxDependencies, 0);
  const int limit   = integerParam(params, "limit", 1, 128, 64);
  const Items items = dependencyItems(context, document);
  if (offset > items.rows.size())
    throw Error("invalid_params", "offset exceeds items_total.",
                {{"items_total", items.rows.size()}});
  const Facts facts    = target.root == "workspace"
                             ? observeWorkspace(context, target)
                             : observeOutputRoot(context, target);
  const Estimate bound = estimate(*document.scene);
  Checks checks;
  checkDocument(checks, document);
  checkPlace(checks, target, facts);
  checkSize(checks, bound, facts);
  checkJournal(checks, context);
  checkExternalPaths(checks, items);
  const int end = std::min(offset + limit, int(items.rows.size()));
  QJsonArray page;
  for (int index = offset; index < end; ++index)
    page.append(items.rows.at(index));
  const QJsonValue failure = checks.firstFailure();
  return {
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"ready", failure.isNull()},
      {"first_failure", failure},
      {"checks", checks.report()},
      {"plan", plan(context, target, bound, facts)},
      {"items", page},
      {"items_total", items.rows.size()},
      {"next_offset", end < items.rows.size() ? QJsonValue(end) : QJsonValue()},
      {"not_contained", packageNotContained()}};
}

}  // namespace GroundzeroAutomation
