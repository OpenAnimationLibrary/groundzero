#include "automation_shared.h"
#include "automation_shared_parse.h"

#include "tfilepath.h"
#include "toonz/txshsimplelevel.h"

#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 SidecarBytes    = 32 * 1024;
constexpr qint64 IconBytes       = 1024 * 1024;
constexpr int IconPixels         = 1024;
constexpr int OutputPixels       = 512;
constexpr int DependencyFiles    = 2048;
constexpr qint64 DependencyBytes = 512LL * 1024 * 1024;

struct ItemKind final {
  QString kind;
  QString folder;
  QString extension;
  qint64 maximum;
  QStringList sidecarKinds;
};

ItemKind itemKind(const QString& kind) {
  if (kind == "template")
    return {kind, "templates", "tnz", 16 * 1024 * 1024, {"rig", "symbol"}};
  if (kind == "pose_set")
    return {kind, "poses", "tposes", 4 * 1024 * 1024, {"poseSet"}};
  throw Error("invalid_params", "kind must be template or pose_set.");
}

QString itemName(const EntryInfo& info, const ItemKind& kind) {
  const QString suffix = '.' + kind.extension;
  const QString name   = info.fileName();
  return name.endsWith(suffix) ? name.chopped(suffix.size()) : QString();
}

// Strict reading of the native libraryItem sidecar (LibraryCmd::readItemInfo).
struct Sidecar final {
  QString id, kind, sourceScene, levelFolder;
  qint64 revision = 0;
};

bool sidecarText(QXmlStreamReader& xml, QHash<QString, QString>& values) {
  static const QStringList known{"id", "revision", "kind", "sourceScene",
                                 "levelFolder"};
  if (!xml.readNextStartElement() || xml.name() != QLatin1String("libraryItem"))
    return false;
  while (xml.readNextStartElement()) {
    const QString tag = xml.name().toString();
    if (!known.contains(tag)) {
      xml.skipCurrentElement();
      continue;
    }
    if (values.contains(tag)) return false;
    values.insert(tag, xml.readElementText());
  }
  while (!xml.atEnd()) xml.readNext();
  return !xml.hasError();
}

bool controlFree(const QString& value) {
  for (const QChar character : value)
    if (character.unicode() < 0x20 || character.unicode() == 0x7f) return false;
  return true;
}

bool parseSidecar(const QByteArray& bytes, Sidecar& sidecar) {
  if (bytes.contains("<!DOCTYPE") || bytes.contains("<!ENTITY")) return false;
  QXmlStreamReader xml(bytes);
  QHash<QString, QString> values;
  if (!sidecarText(xml, values)) return false;
  static const QRegularExpression digits(
      QRegularExpression::anchoredPattern("[0-9]{1,10}"));
  const QString revision = values.value("revision").trimmed();
  sidecar.id             = values.value("id").trimmed();
  sidecar.kind           = values.value("kind").trimmed();
  sidecar.sourceScene    = values.value("sourceScene");
  sidecar.levelFolder    = values.value("levelFolder");
  sidecar.revision       = revision.toLongLong();
  return !sidecar.id.isEmpty() && sidecar.id.size() <= 256 &&
         controlFree(sidecar.id) && digits.match(revision).hasMatch() &&
         sidecar.revision >= 1 && sidecar.revision <= 2000000000 &&
         QStringList{"rig", "symbol", "poseSet"}.contains(sidecar.kind) &&
         sidecar.sourceScene.size() <= 256 &&
         (sidecar.levelFolder.isEmpty() || safeSharedName(sidecar.levelFolder));
}

struct Identity final {
  QString status;
  QJsonObject json;
  QString id;
  QString levelFolder;
};

QJsonObject identityJson(const QString& status, const QJsonValue& sidecar) {
  return {{"status", status},
          {"item_id", QJsonValue()},
          {"revision", QJsonValue()},
          {"kind", QJsonValue()},
          {"source_scene", QJsonValue()},
          {"level_folder", QJsonValue()},
          {"sidecar_file_revision", sidecar}};
}

Identity readIdentity(const QString& directory, const QString& name,
                      const ItemKind& kind) {
  const QString path = directory + '/' + name + ".libinfo";
  const QFileInfo info(path);
  if (!info.exists() && !info.isSymLink())
    return {"not_recorded", identityJson("not_recorded", QJsonValue()), {}, {}};
  SharedFile file;
  try {
    if (info.isSymLink() || info.isJunction()) throw Error("path_denied", "");
    file = readSharedFile(path, SidecarBytes);
  } catch (const Error&) {
    return {"malformed", identityJson("malformed", QJsonValue()), {}, {}};
  }
  Sidecar sidecar;
  if (!parseSidecar(file.bytes, sidecar))
    return {"malformed", identityJson("malformed", file.revision), {}, {}};
  const QString status =
      kind.sidecarKinds.contains(sidecar.kind) ? "recorded" : "kind_mismatch";
  QJsonObject json = identityJson(status, file.revision);
  json.insert("item_id", sidecar.id);
  json.insert("revision", double(sidecar.revision));
  json.insert("kind", sidecar.kind);
  json.insert("source_scene", sidecar.sourceScene);
  json.insert("level_folder", sidecar.levelFolder.isEmpty()
                                  ? QJsonValue()
                                  : QJsonValue(sidecar.levelFolder));
  return {status, json, sidecar.id, sidecar.levelFolder};
}

struct Candidate final {
  EntryInfo info;
  QString name;
  bool oversized = false;
};

// Admitted item files of one kind folder; skipped counts go to the caller.
QVector<Candidate> candidates(const SharedFolder& folder, const ItemKind& kind,
                              int* symlinks = nullptr, int* unsafe = nullptr) {
  QVector<Candidate> result;
  for (const EntryInfo& info : folder.entries) {
    const SharedEntryKind entry = sharedEntryKind(info);
    const bool matches = info.fileName().endsWith('.' + kind.extension);
    if (entry == SharedEntryKind::Symlink && matches && symlinks) ++*symlinks;
    if (entry != SharedEntryKind::File && entry != SharedEntryKind::Unsafe)
      continue;
    if (!matches) continue;
    const QString name = itemName(info, kind);
    if (entry == SharedEntryKind::Unsafe || !info.isFile() ||
        !safeSharedName(name)) {
      if (unsafe) ++*unsafe;
      continue;
    }
    result.append({info, name, info.size() > kind.maximum});
  }
  return result;
}

// Sidecar identities of every Library item, with duplicate ids resolved.
struct LibraryIndex final {
  QHash<QString, Identity> identities;
  QHash<QString, int> counts;

  QJsonObject identity(const Identity& found) const {
    QJsonObject json = found.json;
    if (!found.id.isEmpty() && counts.value(found.id) > 1)
      json.insert("status", "duplicate_id");
    return json;
  }
  QJsonObject identity(const QString& path) const {
    return identity(identities.value(path));
  }
};

LibraryIndex libraryIndex(const QString& root) {
  LibraryIndex index;
  for (const QString& name : {QString("template"), QString("pose_set")}) {
    const ItemKind kind     = itemKind(name);
    const QString directory = root + '/' + kind.folder;
    const QFileInfo info(directory);
    if (!info.isDir() || info.isSymLink() || info.isJunction()) continue;
    for (const Candidate& item :
         candidates(readSharedFolder(directory), kind)) {
      const Identity identity = readIdentity(directory, item.name, kind);
      index.identities.insert(item.info.absoluteFilePath(), identity);
      if (!identity.id.isEmpty()) ++index.counts[identity.id];
    }
  }
  return index;
}

bool pngSize(const QByteArray& bytes, QSize& size) {
  static const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
  if (bytes.size() < 33 || !bytes.startsWith(signature) ||
      bytes.mid(12, 4) != "IHDR")
    return false;
  const auto word = [&bytes](int at) {
    return (quint32(quint8(bytes[at])) << 24) |
           (quint32(quint8(bytes[at + 1])) << 16) |
           (quint32(quint8(bytes[at + 2])) << 8) |
           quint32(quint8(bytes[at + 3]));
  };
  if (word(16) > 1 << 30 || word(20) > 1 << 30) return false;
  size = QSize(int(word(16)), int(word(20)));
  return size.width() >= 1 && size.height() >= 1;
}

struct Icon final {
  QString status = "missing";
  QString path;
  QSize size;
};

Icon templateIcon(const QString& root, const QString& name) {
  Icon icon;
  try {
    icon.path =
        resolveSharedPath(root, {"templates", "sceneIcons", name + " .png"});
  } catch (const Error& error) {
    icon.status = error.code == "not_found" ? "missing" : "invalid";
    return icon;
  }
  icon.status = "invalid";
  try {
    const QByteArray head = readSharedHead(icon.path, 64);
    if (QFileInfo(icon.path).size() <= IconBytes && pngSize(head, icon.size) &&
        icon.size.width() <= IconPixels && icon.size.height() <= IconPixels)
      icon.status = "present";
  } catch (const Error&) {
  }
  return icon;
}

QString libraryFolder(const QString& root, const ItemKind& kind) {
  const QString directory = resolveSharedPath(root, {kind.folder});
  if (!QFileInfo(directory).isDir())
    throw Error("not_found", "The Library has no " + kind.folder + " folder.");
  return directory;
}

QJsonObject itemRow(const Candidate& item, const ItemKind& kind,
                    const QString& root, const LibraryIndex& index,
                    bool collision) {
  const QString thumbnail = kind.kind == "template"
                                ? templateIcon(root, item.name).status
                                : QString("not_applicable");
  return {{"name", item.name},
          {"path", kind.folder + '/' + item.info.fileName()},
          {"size_bytes", double(item.info.size())},
          {"identity", index.identity(item.info.absoluteFilePath())},
          {"thumbnail", thumbnail},
          {"case_fold_collision", collision}};
}

QJsonObject libraryPage(const SharedFolder& listing, const ItemKind& kind,
                        const QString& root, const QJsonObject& params) {
  int symlinks = 0, unsafe = 0, oversized = 0;
  QVector<Candidate> items;
  const QString filter = foldedName(params.value("name_contains").toString());
  QHash<QString, int> folds;
  for (const Candidate& item : candidates(listing, kind, &symlinks, &unsafe)) {
    if (item.oversized) {
      ++oversized;
      continue;
    }
    ++folds[foldedName(item.name)];
    if (foldedName(item.name).contains(filter)) items.append(item);
  }
  std::sort(items.begin(), items.end(),
            [](const Candidate& left, const Candidate& right) {
              const QString a = foldedName(left.name),
                            b = foldedName(right.name);
              return a != b ? a < b : left.name < right.name;
            });
  const LibraryIndex index = libraryIndex(root);
  const int offset = integerParam(params, "offset", 0, SharedFolderEntries, 0);
  const int limit  = integerParam(params, "limit", 1, 256, 128);
  const int end    = std::min<int>(items.size(), offset + limit);
  QJsonArray rows;
  for (int at = offset; at < end; ++at)
    rows.append(itemRow(items[at], kind, root, index,
                        folds.value(foldedName(items[at].name)) > 1));
  return {{"kind", kind.kind},
          {"listing_revision", listing.revision},
          {"total", items.size()},
          {"items", rows},
          {"next_offset", end < items.size() ? QJsonValue(end) : QJsonValue()},
          {"skipped", QJsonObject{{"symlinks", symlinks},
                                  {"unsafe_names", unsafe},
                                  {"oversized", oversized}}}};
}

QJsonObject listItems(Context& context, const QJsonObject& params) {
  const ItemKind kind     = itemKind(requiredString(params, "kind"));
  const QString root      = sharedRootPath(context, "library");
  const QString directory = libraryFolder(root, kind);
  return stableSharedListing(directory, [&](const SharedFolder& listing) {
    requireListingRevision(params, listing);
    return libraryPage(listing, kind, root, params);
  });
}

// One dependency file row and the digest material it contributes.
struct Dependencies final {
  QString root;
  QJsonArray rows;
  QSet<QString> referenced;
  QVector<QByteArray> digest;
  qint64 hashed = 0;
  bool complete = true;

  void add(const QString& reference, const QString& role,
           const QString& location, const QString& relative) {
    if (rows.size() == DependencyFiles)
      throw Error("resource_limit", "The item has more than 2048 files.");
    QJsonObject row{{"reference", reference},
                    {"role", role},
                    {"location", location},
                    {"path", QJsonValue()},
                    {"exists", false},
                    {"size_bytes", QJsonValue()},
                    {"file_revision", QJsonValue()}};
    QString revision = "missing";
    if (!relative.isEmpty()) {
      row.insert("path", relative);
      referenced.insert(relative);
      revision = hashInto(row, relative);
    }
    if (role == "level" && revision == "missing") complete = false;
    rows.append(row);
    digest.append((role + '\t' + reference + '\t' + revision).toUtf8());
  }

  QString hashInto(QJsonObject& row, const QString& relative) {
    QString path;
    try {
      path = resolveSharedPath(root, relative.split('/'));
    } catch (const Error& error) {
      if (error.code != "path_denied") return "missing";
      row.insert("location", "unresolved");
      row.insert("path", QJsonValue());
      return "missing";
    }
    if (!QFileInfo(path).isFile()) return "missing";
    const SharedDigest file = hashSharedFile(path, DependencyBytes - hashed);
    hashed += file.size;
    row.insert("exists", true);
    row.insert("size_bytes", double(file.size));
    row.insert("file_revision", file.revision);
    return file.revision;
  }

  QString digestText() {
    std::sort(digest.begin(), digest.end());
    QByteArray joined;
    for (const QByteArray& line : digest) joined += line + '\n';
    return "deps:" + sha256Revision(joined).mid(7);
  }
};

struct Located final {
  QString location;
  QString relative;
};

Located locate(const QString& root, const QString& reference,
               const QString& itemFolder) {
  const QString value = QDir::fromNativeSeparators(reference);
  QString relative;
  if (value.startsWith("$scenefolder/"))
    relative = "templates/" + value.mid(13);
  else if (QDir::isAbsolutePath(value))
    relative = QDir::cleanPath(value).startsWith(root + '/')
                   ? QDir::cleanPath(value).mid(root.size() + 1)
                   : QString();
  else
    return {"unresolved", {}};
  if (relative.isEmpty()) return {"outside_library", {}};
  for (const QString& part : relative.split('/'))
    if (part.isEmpty() || part == "." || part == "..")
      return {"unresolved", {}};
  const bool own = relative.startsWith("templates/" + itemFolder + '/');
  return {own ? "item_folder" : "library", relative};
}

QStringList levelFiles(const QString& root, const QString& relative) {
  const TFilePath main(relative.toStdWString());
  QStringList files;
  if (!main.isLevelName()) {
    files.append(relative);
  } else {
    const QString parent    = QFileInfo(relative).path();
    const QString directory = root + '/' + parent;
    if (QFileInfo(directory).isDir() && !QFileInfo(directory).isSymLink())
      for (const EntryInfo& entry : readSharedFolder(directory).entries)
        if (sharedEntryKind(entry) == SharedEntryKind::File &&
            TFilePath(entry.fileName().toStdWString()).getLevelNameW() ==
                main.withoutParentDir().getLevelNameW())
          files.append(parent + '/' + entry.fileName());
    std::sort(files.begin(), files.end());
  }
  QStringList extras;
  if (main.getType() == "tlv") extras.append(main.withType("tpl").getQString());
  extras.append(TXshSimpleLevel::getHookPath(main).getQString());
  for (const QString& extra : extras)
    if (QFileInfo(root + '/' + extra).isFile()) files.append(extra);
  if (files.isEmpty()) files.append(relative);
  return files;
}

int unreferencedFiles(const QString& root, const QString& folder,
                      const QSet<QString>& referenced) {
  const QString relative  = "templates/" + folder;
  const QString directory = root + '/' + relative;
  const QFileInfo info(directory);
  if (!info.isDir() || info.isSymLink() || info.isJunction()) return 0;
  int count = 0;
  for (const EntryInfo& entry : readSharedFolder(directory).entries)
    if (sharedEntryKind(entry) == SharedEntryKind::File &&
        !referenced.contains(relative + '/' + entry.fileName()))
      count = std::min(count + 1, DependencyFiles);
  return count;
}

QJsonArray insertionReasons(const QJsonObject& summary,
                            const QJsonArray& levels, const QString& identity) {
  QStringList reasons{"library_insert_unavailable"};
  if (summary.value("child_sheet_count").toInt() > 0)
    reasons.append("child_sheets");
  if (summary.value("pathless_levels").toInt() > 0)
    reasons.append("unsupported_level_type");
  if (summary.value("unrecognized_scene_data").toBool())
    reasons.append("unrecognized_scene_data");
  if (identity != "recorded") reasons.append("identity_not_recorded");
  for (const QJsonValue& value : levels) {
    const QJsonObject level = value.toObject();
    if (level.value("format") == "unsupported")
      reasons.append("unsupported_level_format");
    if (level.value("location") == "outside_library" ||
        level.value("location") == "unresolved")
      reasons.append("external_level_reference");
  }
  reasons.removeDuplicates();
  return QJsonArray::fromStringList(reasons);
}

QJsonObject templateDetails(const QJsonObject& summary, const QString& root,
                            const QString& folder, Dependencies& files,
                            const QString& identity) {
  QJsonArray levels;
  for (const QJsonValue& value : summary.value("levels").toArray()) {
    const QJsonObject level = value.toObject();
    const QString reference = level.value("reference").toString();
    const Located where     = locate(root, reference, folder);
    if (where.relative.isEmpty())
      files.add(reference, "level", where.location, {});
    for (const QString& file : where.relative.isEmpty()
                                   ? QStringList()
                                   : levelFiles(root, where.relative))
      files.add(reference, "level", where.location, file);
    levels.append(
        QJsonObject{{"name", level.value("name").toString().left(256)},
                    {"format", level.value("format")},
                    {"reference", reference.left(4096)},
                    {"location", where.location}});
  }
  return {
      {"frame_count", summary.value("frame_count")},
      {"column_count", summary.value("column_count")},
      {"level_count", levels.size()},
      {"child_sheet_count", summary.value("child_sheet_count")},
      {"levels", levels},
      {"worker_insertable", false},
      {"not_insertable_reasons", insertionReasons(summary, levels, identity)}};
}

QJsonObject poseDetails(const QJsonObject& summary) {
  QJsonArray poses;
  const QJsonArray rows = summary.value("poses").toArray();
  for (int index = 0; index < rows.size(); ++index) {
    QJsonObject pose = rows[index].toObject();
    pose.insert("pose_index", index);
    poses.append(pose);
  }
  return {{"pose_count", poses.size()},
          {"has_base", summary.value("has_base").toBool()},
          {"poses", poses}};
}

QJsonObject ownership(bool isTemplate) {
  return {{"structure",
           isTemplate ? "snapshot_on_insert" : "merged_into_scene_poses"},
          {"level_files_on_copy",
           isTemplate ? "document_local_copy" : "not_applicable"},
          {"level_files_on_link",
           isTemplate ? "reads_library_files" : "not_applicable"},
          {"origin_recorded_on_insert", isTemplate},
          {"live_updates_after_insert", false}};
}

QJsonObject thumbnailState(const QString& root, const QString& name,
                           bool isTemplate) {
  if (!isTemplate)
    return {{"status", "not_applicable"},
            {"width", QJsonValue()},
            {"height", QJsonValue()}};
  const Icon icon    = templateIcon(root, name);
  const bool present = icon.status == "present";
  return {{"status", icon.status},
          {"width", present ? QJsonValue(icon.size.width()) : QJsonValue()},
          {"height", present ? QJsonValue(icon.size.height()) : QJsonValue()}};
}

QString requireItemName(const QJsonObject& params) {
  const QString name = requiredString(params, "name");
  if (!safeSharedName(name))
    throw Error("invalid_path", "Library item names follow the shared rules.");
  return name;
}

QJsonObject inspectItem(Context& context, const QJsonObject& params) {
  const ItemKind kind        = itemKind(requiredString(params, "kind"));
  const QString name         = requireItemName(params);
  const QString root         = sharedRootPath(context, "library");
  const QString file         = name + '.' + kind.extension;
  const QString path         = resolveSharedPath(root, {kind.folder, file});
  const SharedFile item      = readSharedFile(path, kind.maximum);
  const QJsonObject identity = libraryIndex(root).identity(
      readIdentity(root + '/' + kind.folder, name, kind));
  const bool isTemplate = kind.kind == "template";
  const QJsonObject summary =
      parseSharedFile(context, isTemplate ? "template" : "pose_set", item.bytes,
                      "invalid_library_item");
  Dependencies files{root};
  files.add(kind.folder + '/' + file,
            isTemplate ? "template_scene" : "pose_set", "library",
            kind.folder + '/' + file);
  files.add(kind.folder + '/' + name + ".libinfo", "sidecar", "library",
            kind.folder + '/' + name + ".libinfo");
  if (isTemplate)
    files.add("templates/sceneIcons/" + name + " .png", "thumbnail", "library",
              "templates/sceneIcons/" + name + " .png");
  const QString folder = identity.value("level_folder").isString()
                             ? identity.value("level_folder").toString()
                             : name;
  const QJsonValue details =
      isTemplate
          ? QJsonValue(templateDetails(summary, root, folder, files,
                                       identity.value("status").toString()))
          : QJsonValue();
  return {{"kind", kind.kind},
          {"name", name},
          {"path", kind.folder + '/' + file},
          {"file_revision", item.revision},
          {"identity", identity},
          {"ownership", ownership(isTemplate)},
          {"template", details},
          {"pose_set", isTemplate ? QJsonValue() : poseDetails(summary)},
          {"thumbnail", thumbnailState(root, name, isTemplate)},
          {"dependencies", files.rows},
          {"dependency_digest", files.digestText()},
          {"dependencies_complete", files.complete},
          {"unreferenced_level_folder_files",
           isTemplate ? unreferencedFiles(root, folder, files.referenced) : 0}};
}

QJsonObject encodedImage(const QByteArray& png) {
  QSize size;
  if (png.size() > IconBytes)
    throw Error("resource_limit", "The thumbnail exceeds 1 MiB.");
  if (!pngSize(png, size))
    throw Error("asset_unavailable", "The thumbnail is not a PNG image.",
                {{"reason", "invalid"}});
  if (size.width() > IconPixels || size.height() > IconPixels)
    throw Error("resource_limit", "The thumbnail exceeds 1024 pixels.");
  QImage image;
  if (!image.loadFromData(png, "PNG"))
    throw Error("asset_unavailable", "The thumbnail cannot be decoded.",
                {{"reason", "invalid"}});
  if (image.width() > OutputPixels || image.height() > OutputPixels)
    image = image.scaled(OutputPixels, OutputPixels, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
  QByteArray encoded;
  QBuffer buffer(&encoded);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
    throw Error("io_error", "Cannot encode the thumbnail.");
  return {{"width", image.width()},
          {"height", image.height()},
          {"image",
           QJsonObject{{"mimeType", "image/png"},
                       {"data", QString::fromLatin1(encoded.toBase64())}}}};
}

QJsonObject poseThumbnail(Context& context, const SharedFile& item, int pose) {
  const QJsonObject summary =
      parseSharedFile(context, "pose_thumbnail:" + QString::number(pose),
                      item.bytes, "invalid_library_item");
  if (pose >= summary.value("poses").toArray().size())
    throw Error("not_found", "The pose set has no pose at this index.");
  if (!summary.value("thumbnail").isString())
    throw Error("asset_unavailable", "The pose has no thumbnail.",
                {{"reason", "missing"}});
  return encodedImage(
      QByteArray::fromBase64(summary.value("thumbnail").toString().toLatin1()));
}

QJsonObject itemThumbnail(Context& context, const QJsonObject& params) {
  const ItemKind kind = itemKind(requiredString(params, "kind"));
  const QString name  = requireItemName(params);
  const bool hasPose  = params.contains("pose_index");
  if (hasPose == (kind.kind == "template"))
    throw Error("invalid_params",
                "pose_index is required for pose sets and refused for "
                "templates.");
  const QString root = sharedRootPath(context, "library");
  const QString path =
      resolveSharedPath(root, {kind.folder, name + '.' + kind.extension});
  QJsonObject result;
  QString revision;
  if (hasPose) {
    const SharedFile item = readSharedFile(path, kind.maximum);
    const int pose        = integerParam(params, "pose_index", 0, 1023, 0);
    result                = poseThumbnail(context, item, pose);
    revision              = item.revision;
    result.insert("pose_index", pose);
  } else {
    const Icon icon = templateIcon(root, name);
    if (icon.status == "missing")
      throw Error("asset_unavailable", "The template has no thumbnail.",
                  {{"reason", "missing"}});
    if (icon.path.isEmpty())
      throw Error("asset_unavailable",
                  "The template thumbnail is a link or cannot be resolved.",
                  {{"reason", "unavailable"}});
    const SharedFile file = readSharedFile(icon.path, IconBytes);
    result                = encodedImage(file.bytes);
    revision              = file.revision;
    result.insert("pose_index", QJsonValue());
  }
  result.insert("kind", kind.kind);
  result.insert("name", name);
  result.insert("source_file_revision", revision);
  return result;
}

QJsonObject kindSchema() {
  return {{"type", "string"}, {"enum", QJsonArray{"template", "pose_set"}}};
}

QJsonObject nameSchema() {
  return {{"type", "string"},
          {"minLength", 1},
          {"maxLength", 120},
          {"pattern", "^" + sharedSegmentPattern() + "$"}};
}

}  // namespace

QJsonArray libraryOperations() {
  return {
      sharedDescriptor(
          "library.list",
          "List Library templates (rigs/symbols) or pose sets with sidecar "
          "identity, revision, thumbnail state and name collisions. Bounded, "
          "paged, and never inserts an item.",
          {{"kind", kindSchema()},
           {"name_contains",
            QJsonObject{
                {"type", "string"}, {"minLength", 1}, {"maxLength", 120}}},
           {"offset", sharedIntegerSchema(0, 8192)},
           {"limit", sharedIntegerSchema(1, 256)},
           {"expected_listing_revision", sharedRevisionSchema("listing")}},
          {"kind"}),
      sharedDescriptor(
          "library.inspect",
          "Inspect one Library item: sidecar identity and revision, ownership "
          "semantics, structure summary, dependency files with digests and a "
          "dependency digest for insertion preconditions. Parses a private "
          "copy; never inserts.",
          {{"kind", kindSchema()}, {"name", nameSchema()}}, {"kind", "name"}),
      sharedDescriptor(
          "library.thumbnail",
          "Return a Library item's thumbnail (template icon or one pose "
          "thumbnail) as a re-encoded PNG of at most 512 pixels per side.",
          {{"kind", kindSchema()},
           {"name", nameSchema()},
           {"pose_index", sharedIntegerSchema(0, 1023)}},
          {"kind", "name"})};
}

QJsonValue executeLibrary(Context& context, const QString& method,
                          const QJsonObject& params) {
  if (method == "library.list") return listItems(context, params);
  if (method == "library.inspect") return inspectItem(context, params);
  if (method == "library.thumbnail") return itemThumbnail(context, params);
  throw Error("method_not_found", "The Library operation is not available.");
}

}  // namespace GroundzeroAutomation
