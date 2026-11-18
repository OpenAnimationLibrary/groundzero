#include "automation.h"
#include "automation_dispatch.h"
#include "automation_scene_host.h"

#include "tfilepath.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QJsonObject call(Dispatcher& dispatcher, const QString& method,
                 const QJsonObject& params) {
  return dispatcher.dispatch(
      {{"id", "scene-host-test"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const QJsonObject response = call(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}

QJsonObject failure(Dispatcher& dispatcher, const QString& method,
                    const QJsonObject& params, const QString& code) {
  const QJsonObject error =
      call(dispatcher, method, params).value("error").toObject();
  if (error.value("code").toString() != code)
    throw std::runtime_error(
        (method + " expected " + code + ": " +
         QJsonDocument(error).toJson(QJsonDocument::Compact))
            .toStdString());
  return error.value("details").toObject();
}

Error caught(const std::function<void()>& action) {
  try {
    action();
  } catch (const Error& error) {
    return error;
  }
  return Error("none", "No error was raised.");
}

void write(const QString& path, const QByteArray& bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "Cannot write a scene host fixture");
}

QByteArray projectXml(const QByteArray& folders = {}) {
  return "<project><version>70 1</version><folders>" + folders +
         "</folders></project>\n";
}

// Pins are order independent and change with size, time and identity.
void checkPins(const QString& root) {
  const QString first  = root + "/pins/a.bin";
  const QString second = root + "/pins/b.bin";
  write(first, "one");
  write(second, "two");
  const std::vector<SourcePin> forward{statSourcePin(first),
                                       statSourcePin(second)};
  const std::vector<SourcePin> backward{forward[1], forward[0]};
  require(sourceFingerprint({"scene x"}, forward) ==
              sourceFingerprint({"scene x"}, backward),
          "The source fingerprint is order independent");
  require(sourceFingerprint({"scene x"}, forward) !=
              sourceFingerprint({"scene y"}, forward),
          "The fingerprint covers the source revisions");
  require(changedSourcePins(forward).isEmpty(), "Unchanged pins stay equal");
  write(first, "one more");
  require(changedSourcePins(forward) == QStringList{first},
          "A size change is a changed pin");
  std::vector<SourcePin> moved{statSourcePin(second)};
  moved[0].fileId += 1;
  require(changedSourcePins(moved) == QStringList{second},
          "A file identity change is a changed pin");
  std::vector<SourcePin> timed{statSourcePin(second)};
  timed[0].mtimeNs -= 1;
  require(changedSourcePins(timed) == QStringList{second},
          "A modification time change is a changed pin");
  QFile::remove(second);
  require(
      caught([&] { requireUnchangedSources(timed); }).code == "source_changed",
      "A removed pin is source_changed");
}

// A file whose folder cannot be searched is unreadable, not missing.
void checkUnreadablePin(const QString& root) {
#ifndef _WIN32
  const QString folder = root + "/closed";
  write(folder + "/level.png", "pixels");
  QFile::setPermissions(folder, QFileDevice::Permissions());
  const SourcePin pin = statSourcePin(folder + "/level.png");
  QFile::setPermissions(folder, QFileDevice::ReadOwner |
                                    QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner);
  require(!pin.exists && pin.unreadable,
          "A pin behind a closed folder is unreadable");
  require(!statSourcePin(root + "/absent.png").unreadable,
          "A missing pin is not unreadable");
#else
  Q_UNUSED(root);
#endif
}

void checkPinLimit(const QString& root) {
  ReadOnlyScene source;
  for (int index = 0; index < PinnedFileLimit; ++index)
    addSourcePin(source, root + "/missing/" + QString::number(index));
  addSourcePin(source, root + "/missing/0");
  require(int(source.pins.size()) == PinnedFileLimit,
          "65,536 distinct pins are admitted and repeats are not counted");
  const Error error =
      caught([&] { addSourcePin(source, root + "/missing/over"); });
  require(error.code == "scene_limit" &&
              error.details.value("limit") == "pinned_files",
          "The 65,537th pinned file is a scene_limit");
}

QJsonObject projectRow(const QJsonObject& listing, const QString& folder) {
  for (const QJsonValue& value : listing.value("projects").toArray())
    if (value.toObject().value("folder") == folder) return value.toObject();
  throw std::runtime_error("project.list omitted a project folder");
}

// project.list picks the file native getProjectFile picks, never parsing it.
void checkDiscovery(Dispatcher& dispatcher, const QString& root) {
  const QString base = root + "/discovery";
  write(base + "/Alpha/Alpha_prj6.xml", projectXml());
  write(base + "/Alpha/Alpha_otprj.xml", "not xml at all");
  write(base + "/Beta/tahomaproject.xml", projectXml());
  write(base + "/Beta/Beta_prj.xml", projectXml());
  write(base + "/Gamma/tahomaproject.xml", projectXml());
  write(base + "/Delta/Other_otprj.xml", projectXml());
  write(base + "/deep/a/b/c/d/Hidden/Hidden_otprj.xml", projectXml());
  write(base + "/Large/Large_otprj.xml", QByteArray(1024 * 1024 + 1, 'x'));
  const QJsonObject listing =
      result(dispatcher, "project.list", {{"folder", base}, {"limit", 64}});
  for (const QString& name : {"Alpha", "Beta", "Gamma", "Delta"}) {
    const QString folder = base + "/" + name;
    const QString expected =
        TProjectManager::instance()
            ->projectFolderToProjectPath(TFilePath(folder.toStdWString()))
            .getQString();
    require(projectRow(listing, folder).value("project_file") == expected,
            "project.list picks the native project file");
  }
  const QJsonObject alpha = projectRow(listing, base + "/Alpha");
  require(alpha.value("format") == "otprj" &&
              alpha.value("alternates").toArray().size() == 1,
          "The newest suffix wins and other project files are alternates");
  require(projectRow(listing, base + "/Beta").value("format") == "prj",
          "OpenToonz project files precede tahomaproject.xml");
  require(projectRow(listing, base + "/Gamma").value("format") == "tahoma",
          "A lone tahomaproject.xml is a project");
  require(projectRow(listing, base + "/Large").value("status") == "too_large",
          "Project files above 1 MiB are listed unhashed");
  require(listing.value("scan").toObject().value("reasons").toArray().contains(
              "depth_limit"),
          "Folders below depth 4 are reported, not scanned");
  require(alpha.value("project_id").toString() ==
              projectIdForFile(alpha.value("project_file").toString()),
          "project_id hashes the canonical project file path");
  failure(dispatcher, "project.list",
          {{"folder", base}, {"expected_listing_revision", "listing:0"}},
          "revision_conflict");
  failure(dispatcher, "project.list", {{"folder", base}, {"offset", 4000}},
          "invalid_params");
}

HostLoadRequest request(Context& context, const QString& scene,
                        const QString& projectId) {
  HostLoadRequest load;
  load.scenePath       = scene;
  load.sceneCopy       = scene;
  load.projectId       = projectId;
  load.binding         = "must_match_native";
  load.workerWorkspace = context.workspace();
  load.readRoots = QStringList{context.workspace()} + context.inputRoots();
  return load;
}

QString bindingStatus(Context& context, HostLoadRequest load) {
  const Error error = caught(
      [&] { loadReadOnlyDocument(context, "document_binding_test", load); });
  if (error.code != "project_mismatch")
    throw std::runtime_error(
        ("Expected project_mismatch: " + error.code + " " + error.message +
         " " + QJsonDocument(error.details).toJson(QJsonDocument::Compact))
            .toStdString());
  return error.details.value("native_binding").toString();
}

void scenesXml(HostLoadRequest& load, const QString& path,
               const QByteArray& bytes) {
  write(path, bytes);
  load.scenesXml         = path;
  load.scenesXmlRevision = fileRevision(bytes);
}

void requireStatus(Context& context, const HostLoadRequest& load,
                   const QString& expected, const char* message) {
  const QString status = bindingStatus(context, load);
  if (status != expected)
    throw std::runtime_error(std::string(message) + " (got " +
                             status.toStdString() + ")");
}

// The native binding statuses that refuse a must_match_native open.
void checkBinding(Context& context, const QString& root) {
  const QString base    = root + "/binding";
  const QString scene   = base + "/Show/scenes/shot.tnz";
  const QString project = base + "/Show/Show_otprj.xml";
  write(scene, "<tnz/>");
  write(project, projectXml());
  const QString showId =
      projectIdForFile(QFileInfo(project).canonicalFilePath());
  HostLoadRequest load = request(context, scene, "project_other");
  scenesXml(load, base + "/Show/scenes/scenes.xml",
            "<parentProject type=\"projectFolder\">\"..\"</parentProject>\n");
  requireStatus(context, load, "differs",
                "A scenes.xml naming another project differs");
  scenesXml(load, base + "/Show/scenes/scenes.xml",
            "<parentProject type=\"projectFolder\">\"/nowhere/at/all\""
            "</parentProject>\n");
  requireStatus(context, load, "outside_roots",
                "A project outside the roots is outside_roots and never read");
  scenesXml(load, base + "/Show/scenes/scenes.xml", "<parentProject");
  requireStatus(context, load, "unreadable",
                "A damaged scenes.xml is unreadable");
  scenesXml(load, base + "/Show/scenes/scenes.xml",
            "<parentProject type=\"projectName\">Show</parentProject>\n");
  requireStatus(context, load, "unresolved_name",
                "A project named only by name is unresolved_name");
  HostLoadRequest bare = request(context, scene, showId);
  requireStatus(context, bare, "none_within_roots",
                "No scenes.xml within the roots is none_within_roots");
  HostLoadRequest matched = request(context, scene, showId);
  scenesXml(matched, base + "/Show/scenes/scenes.xml",
            "<parentProject type=\"projectFolder\">\"..\"</parentProject>\n");
  matched.projectFile     = QFileInfo(project).canonicalFilePath();
  matched.projectRevision = fileRevision(projectXml());
  matched.sceneRevision   = fileRevision("<tnz/>");
  const Error rejected    = caught(
      [&] { loadReadOnlyDocument(context, "document_binding_test", matched); });
  require(rejected.code == "invalid_scene" &&
              rejected.details.value("cause") == "rejected",
          "A matching binding reaches the scene parser, which rejects it");
  for (const QJsonValue& row : context.documents())
    require(row.toObject().value("document_id") != "document_binding_test",
            "A failed open registers no document");
  HostLoadRequest stale = load;
  scenesXml(stale, base + "/Show/scenes/scenes.xml",
            "<parentProject type=\"projectFolder\">\"..\"</parentProject>\n");
  stale.scenesXmlRevision = fileRevision("changed");
  require(caught([&] {
            loadReadOnlyDocument(context, "document_binding_test", stale);
          }).code == "source_changed",
          "A scenes.xml that changed after the worker hashed it is refused");
}

std::unique_ptr<ToonzScene> nestedScene(int depth) {
  auto scene     = std::make_unique<ToonzScene>();
  TXsheet* sheet = scene->getXsheet();
  for (int level = 0; level < depth; ++level) {
    TXshLevel* created = scene->createNewLevel(
        CHILD_XSHLEVEL, L"Child" + std::to_wstring(level));
    TXshChildLevel* child = created->getChildLevel();
    sheet->setCell(0, 0, TXshCell(child, TFrameId(1)));
    sheet = child->getXsheet();
  }
  return scene;
}

void checkStructure() {
  auto deep = nestedScene(SheetDepthLimit);
  requireReadOnlyStructure(*deep);
  auto deeper       = nestedScene(SheetDepthLimit + 1);
  const Error error = caught([&] { requireReadOnlyStructure(*deeper); });
  require(error.code == "scene_limit" &&
              error.details.value("limit") == "sheet_depth",
          "Sub-xsheets nest 16 deep and no deeper");
}

// An empty scene saved through the worker, as user scene material.
QString packageScene(Dispatcher& dispatcher, const QString& root) {
  const QJsonObject created =
      result(dispatcher, "scene.create",
             {{"name", "HostSource"}, {"operation_id", "scene-host-create"}});
  const QString document = created.value("document_id").toString();
  const QJsonObject saved =
      result(dispatcher, "scene.save_copy",
             {{"document_id", document},
              {"expected_revision", created.value("revision")},
              {"directory", root + "/package"},
              {"operation_id", "scene-host-save"}});
  result(dispatcher, "scene.discard",
         {{"document_id", document},
          {"expected_revision", saved.value("revision")},
          {"operation_id", "scene-host-discard"}});
  QFile scene(saved.value("scene_path").toString());
  require(scene.open(QIODevice::ReadOnly), "Cannot read the package scene");
  return QString::fromUtf8(scene.readAll());
}

HostLoadRequest sceneRequest(Context& context, const QString& scene,
                             const QString& text, const QString& projectId) {
  write(scene, text.toUtf8());
  HostLoadRequest load = request(context, scene, projectId);
  load.sceneRevision   = fileRevision(text.toUtf8());
  return load;
}

QJsonObject folderRows(const QJsonObject& report) {
  QJsonObject rows;
  for (const QJsonValue& value :
       report.value("project").toObject().value("folders").toArray())
    rows.insert(value.toObject().value("alias").toString(), value);
  return rows;
}

// Folder aliases resolve against the bound project; outside the roots they are
// withheld, never stat-ed.
void checkAliases(Context& context, const QString& root, const QString& text) {
  const QString show    = root + "/alias/Show";
  const QString project = show + "/Show_otprj.xml";
  const QByteArray folders =
      "<folder name=\"drawings\" path=\"" + show.toUtf8() +
      "/art\"/><folder name=\"extras\" path=\"extras\"/>"
      "<folder name=\"inputs\" path=\"../Shared\"/>"
      "<folder name=\"outputs\" path=\"/nowhere/outputs\"/>"
      "<folder name=\"palettes\" path=\"$scenepath/palettes\"/>"
      "<folder name=\"scenes\" path=\"scenes\"/>";
  write(project, projectXml(folders));
  const QString canonical = QFileInfo(project).canonicalFilePath();
  HostLoadRequest load = sceneRequest(context, show + "/scenes/shot.tnz", text,
                                      projectIdForFile(canonical));
  scenesXml(load, show + "/scenes/scenes.xml",
            "<parentProject type=\"projectFolder\">\"..\"</parentProject>\n");
  load.projectFile     = canonical;
  load.projectRevision = fileRevision(projectXml(folders));
  const QJsonObject report =
      loadReadOnlyDocument(context, "document_alias_test", load);
  context.removeDocument("document_alias_test");
  const QJsonObject rows = folderRows(report);
  const auto row         = [&rows](const char* alias) {
    return rows.value(alias).toObject();
  };
  require(row("drawings").value("resolved_path") == show + "/art" &&
              row("drawings").value("authority") == "workspace",
          "An absolute folder inside the roots resolves");
  require(row("extras").value("resolved_path") == show + "/extras",
          "A relative folder resolves against the project folder");
  require(row("inputs").value("resolved_path") == root + "/alias/Shared",
          "A .. folder that stays inside the roots resolves");
  require(row("outputs").value("authority") == "outside_roots" &&
              row("outputs").value("resolved_path").isNull() &&
              row("outputs").value("exists").isNull(),
          "A folder outside the roots is withheld");
  require(
      row("palettes").value("scene_dependent").toBool() &&
          row("palettes")
              .value("resolved_path")
              .toString()
              .startsWith(show + "/") &&
          row("palettes").value("resolved_path").toString().contains("shot"),
      "A scene-dependent folder resolves for the open scene");
  require(report.value("project").toObject().value("binding") == "native",
          "The matching project binds natively");
}

QString withLevels(QString text, int count) {
  QString levels;
  for (int index = 0; index < count; ++index)
    levels += QString(
                  "<level id='%1'>\nL%1 \n<path> \"$scenefolder/missing/"
                  "l%1..png\" </path>\n</level>\n")
                  .arg(9000 + index);
  return text.replace("<levels>", "<levels>\n" + levels);
}

// 4096 levels open and every missing one is a row; 4097 levels do not.
void checkLevelLimit(Context& context, const QString& root,
                     const QString& text) {
  const QString scene = root + "/levels/shot.tnz";
  HostLoadRequest load =
      sceneRequest(context, scene, withLevels(text, LevelLimit), "standalone");
  const QJsonObject report =
      loadReadOnlyDocument(context, "document_level_test", load);
  context.removeDocument("document_level_test");
  const QJsonObject content = report.value("content").toObject();
  require(
      report.value("counts").toObject().value("levels") == LevelLimit &&
          content.value("unsupported_count") == LevelLimit &&
          content.value("truncated").toBool() &&
          content.value("unsupported").toArray().size() == UnsupportedRowLimit,
      "4096 levels open with every missing level reported");
  require(report.value("render").toObject().value("renderable").toBool(),
          "Missing levels that are never exposed do not block renders");
  load = sceneRequest(context, scene, withLevels(text, LevelLimit + 1),
                      "standalone");
  const Error error = caught(
      [&] { loadReadOnlyDocument(context, "document_level_test", load); });
  require(
      error.code == "scene_limit" && error.details.value("limit") == "levels",
      "4097 levels are a scene_limit");
}

}  // namespace

void runAutomationSceneHostTests(Context& context) {
  Dispatcher dispatcher(context);
  const QString root = context.resolvePath(
      "scene-host-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces),
      true);
  checkPins(root);
  checkUnreadablePin(root);
  checkPinLimit(root);
  checkDiscovery(dispatcher, root);
  checkBinding(context, root);
  checkStructure();
  const QString text = packageScene(dispatcher, root);
  checkAliases(context, root, text);
  checkLevelLimit(context, root, text);
  TFilePath::setFilePathProperties(false, false, 8, false);
  QDir(root).removeRecursively();
  std::cout << "automation-scene-host-test: pins, discovery, binding, aliases "
               "and limits PASS\n";
}

}  // namespace GroundzeroAutomation
