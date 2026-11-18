#include "automation_scene_host.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>

#include <algorithm>

#ifdef _WIN32
#include <qt_windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace GroundzeroAutomation {

QString readOnlyProfileName() { return "native-read-only-v1"; }

QString linkedSnapshotInputs() { return "linked-source-stat-pinned"; }

QString fileRevision(const QByteArray& bytes) {
  return "sha256:" + QString::fromLatin1(QCryptographicHash::hash(
                                             bytes, QCryptographicHash::Sha256)
                                             .toHex());
}

QString projectIdForFile(const QString& canonicalProjectFile) {
  const QByteArray digest =
      QCryptographicHash::hash(canonicalProjectFile.toUtf8(),
                               QCryptographicHash::Sha256)
          .toHex();
  return "project_" + QString::fromLatin1(digest.left(32));
}

bool SourcePin::operator==(const SourcePin& other) const {
  return path == other.path && size == other.size && mtimeNs == other.mtimeNs &&
         fileId == other.fileId && exists == other.exists;
}

SourcePin statSourcePin(const QString& path) {
  SourcePin pin;
  pin.path = path;
#ifdef _WIN32
  const HANDLE file = CreateFileW(
      reinterpret_cast<const wchar_t*>(path.utf16()), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    pin.unreadable =
        error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    return pin;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  const BOOL read = GetFileInformationByHandle(file, &info);
  CloseHandle(file);
  if (!read) return pin;
  pin.exists = true;
  pin.size   = qint64((quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
  const quint64 ticks = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32) |
                        info.ftLastWriteTime.dwLowDateTime;
  pin.mtimeNs         = qint64(ticks * 100);
  pin.fileId = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
#else
  struct stat info{};
  if (lstat(QFile::encodeName(path).constData(), &info) != 0) {
    pin.unreadable = errno != ENOENT && errno != ENOTDIR;
    return pin;
  }
  pin.exists = true;
  pin.size   = qint64(info.st_size);
#ifdef __APPLE__
  pin.mtimeNs = qint64(info.st_mtimespec.tv_sec) * 1000000000LL +
                info.st_mtimespec.tv_nsec;
#else
  pin.mtimeNs =
      qint64(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec;
#endif
  pin.fileId = quint64(info.st_ino);
#endif
  return pin;
}

void addSourcePin(ReadOnlyScene& source, const QString& path) {
  if (source.pinnedPaths.contains(path)) return;
  if (int(source.pins.size()) >= PinnedFileLimit)
    throw Error(
        "scene_limit", "The scene reads more than 65,536 files.",
        {{"limit", "pinned_files"}, {"value", int(source.pins.size()) + 1}});
  source.pinnedPaths.insert(path);
  source.pins.push_back(statSourcePin(path));
}

QString sourceFingerprint(const QStringList& revisions,
                          const std::vector<SourcePin>& pins) {
  QStringList lines;
  for (const SourcePin& pin : pins)
    lines.append(QString("%1\t%2\t%3\t%4\t%5")
                     .arg(pin.path)
                     .arg(pin.exists ? 1 : 0)
                     .arg(pin.size)
                     .arg(pin.mtimeNs)
                     .arg(pin.fileId));
  std::sort(lines.begin(), lines.end());
  QByteArray canonical = revisions.join('\n').toUtf8();
  canonical.append('\n');
  canonical.append(lines.join('\n').toUtf8());
  return "source:" +
         QString::fromLatin1(
             QCryptographicHash::hash(canonical, QCryptographicHash::Sha256)
                 .toHex());
}

QStringList changedSourcePins(const std::vector<SourcePin>& pins) {
  QStringList changed;
  for (const SourcePin& pin : pins)
    if (statSourcePin(pin.path) != pin) changed.append(pin.path);
  return changed;
}

void requireUnchangedSources(const std::vector<SourcePin>& pins) {
  const QStringList changed = changedSourcePins(pins);
  if (changed.isEmpty()) return;
  throw Error(
      "source_changed",
      "A pinned source file changed since the scene was opened; "
      "discard the document and open it again.",
      {{"paths", QJsonArray::fromStringList(changed.mid(0, ChangedPathLimit))},
       {"count", changed.size()}});
}

namespace {

QString pinChange(const SourcePin& pinned, const SourcePin& current) {
  if (!current.exists) return current.unreadable ? "unreadable" : "missing";
  if (current.fileId != pinned.fileId) return "replaced";
  return "modified";
}

}  // namespace

void requireUnchangedLevel(Document& document, TXshLevel* level) {
  const ReadOnlyScene& source = *document.readOnly;
  const LevelState state      = source.levels.value(level);
  std::vector<SourcePin> pins;
  for (int index = state.firstPin;
       index < state.endPin && index < int(source.pins.size()); ++index)
    pins.push_back(source.pins[index]);
  requireUnchangedSources(pins);
}

QJsonObject sourceStatus(Document& document) {
  QElapsedTimer timer;
  timer.start();
  const ReadOnlyScene& source = *document.readOnly;
  std::vector<SourcePin> current;
  QJsonArray changed;
  int count      = 0;
  bool sceneGone = false;
  for (const SourcePin& pin : source.pins) {
    current.push_back(statSourcePin(pin.path));
    if (current.back() == pin) continue;
    ++count;
    if (pin.path == source.scenePath && !current.back().exists)
      sceneGone = true;
    if (changed.size() < ChangedPathLimit)
      changed.append(QJsonObject{{"path", pin.path},
                                 {"change", pinChange(pin, current.back())}});
  }
  const QString state = sceneGone ? "missing" : count ? "changed" : "unchanged";
  return {{"document_id", document.id},
          {"state", state},
          {"fingerprint_at_open", source.fingerprint},
          {"current_fingerprint", sceneGone ? QJsonValue()
                                            : QJsonValue(sourceFingerprint(
                                                  source.revisions, current))},
          {"changed", changed},
          {"changed_count", count},
          {"checked_files", int(current.size())},
          {"elapsed_ms", double(timer.elapsed())}};
}

namespace {

QJsonValue optionalText(const QString& value) {
  return value.isEmpty() ? QJsonValue() : QJsonValue(value);
}

}  // namespace

QJsonObject ContentRow::unsupportedRow() const {
  return {{"kind", kind},
          {"reason", reason},
          {"name", optionalText(name.left(256))},
          {"level_id", optionalText(levelId)},
          {"column_id", optionalText(columnId)},
          {"fx_id", optionalText(fxId)},
          {"sheet_path", sheetPath},
          {"detail", optionalText(detail.left(512))}};
}

QJsonObject ContentRow::blockerRow() const {
  QString ruleText = "rendered";
  if (rule == Rule::Anywhere) ruleText = "anywhere";
  if (rule == Rule::Camera) ruleText = "camera";
  return {{"reason", reason},
          {"rule", ruleText},
          {"level_id", optionalText(levelId)},
          {"column_id", optionalText(columnId)},
          {"fx_id", optionalText(fxId)},
          {"sheet_path", sheetPath},
          {"detail", optionalText(detail.left(512))}};
}

QJsonObject ReadOnlyScene::summarySource() const {
  return {{"scene_path", scenePath},
          {"source_fingerprint", fingerprint},
          {"project_id", projectId}};
}

}  // namespace GroundzeroAutomation
