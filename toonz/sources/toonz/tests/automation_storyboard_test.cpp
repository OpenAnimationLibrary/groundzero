#include "automation.h"
#include "automation_dispatch.h"
#include "automation_storyboard.h"

#include "toonz/navigationtags.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"

#include <QDir>
#include <QJsonDocument>

#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QJsonObject request(Dispatcher& dispatcher, const QString& method,
                    QJsonObject params) {
  static int sequence = 0;
  if (method != "storyboard.panels")
    params.insert("operation_id", "panel-test-" + QString::number(++sequence));
  return dispatcher.dispatch(
      {{"id", "panel"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const auto response = request(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}

QJsonObject addressed(Document& document, QJsonObject params = {}) {
  params.insert("document_id", document.id);
  params.insert("expected_revision", document.revisionText());
  return params;
}

QString rejected(Dispatcher& dispatcher, Document& document,
                 const QString& method, const QJsonObject& params) {
  const auto revision = document.revision;
  const auto tags = document.scene->getTopXsheet()->getNavigationTags()->m_tags;
  const auto response = request(dispatcher, method, params);
  const auto after =
      document.scene->getTopXsheet()->getNavigationTags()->m_tags;
  require(document.revision == revision && after.size() == tags.size(),
          "A panel rejection changed the document");
  return response.value("error").toObject().value("code").toString();
}

Document& createDocument(Context& context, Dispatcher& dispatcher) {
  const auto created = result(dispatcher, "scene.create",
                              {{"name", "Storyboard tests"},
                               {"width_px", 32},
                               {"height_px", 32},
                               {"frame_rate", 24}});
  return context.document(created.value("document_id").toString());
}

QJsonObject color(int r, int g, int b) {
  return {{"r", r}, {"g", g}, {"b", b}};
}

QJsonObject createParams(Document& document, int frame, const QString& caption,
                         const QString& notes = QString()) {
  return addressed(document, {{"panel_frame", frame},
                              {"caption", caption},
                              {"notes", notes},
                              {"color", color(frame % 256, 10, 200)}});
}

QString exposeDrawing(Dispatcher& dispatcher, Document& document,
                      const QString& name, int length) {
  const QString level =
      result(dispatcher, "level.create",
             addressed(document, {{"name", name}, {"format", "vector"}}))
          .value("level_id")
          .toString();
  const QJsonObject drawing{{"number", 1}, {"suffix", ""}};
  result(dispatcher, "drawing.create",
         addressed(document, {{"level_id", level},
                              {"drawing_id", drawing},
                              {"format", "vector"}}));
  const QString column =
      result(dispatcher, "column.create", addressed(document, {{"name", name}}))
          .value("column_id")
          .toString();
  result(dispatcher, "column.expose",
         addressed(document, {{"column_id", column},
                              {"level_id", level},
                              {"drawing_id", drawing},
                              {"start_frame", 0},
                              {"length", length}}));
  return level;
}

void spansAndThumbnails(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QString shown = exposeDrawing(dispatcher, document, "Shown", 12);
  exposeDrawing(dispatcher, document, "Hidden", 12);
  const QString hidden =
      document.idForColumn(document.scene->getTopXsheet()->getColumn(1));
  result(dispatcher, "column.update",
         addressed(document, {{"column_id", hidden}, {"visible", false}}));
  for (int frame : {4, 0, 20})
    result(dispatcher, "storyboard.panel.create",
           createParams(document, frame, "Panel"));
  const auto panels =
      result(dispatcher, "storyboard.panels", addressed(document))
          .value("panels")
          .toArray();
  require(panels.size() == 3 &&
              panels.at(0).toObject().value("end_frame").toInt() == 3 &&
              panels.at(1).toObject().value("end_frame").toInt() == 19 &&
              panels.at(2).toObject().value("end_frame").toInt() == 20,
          "Panel spans differ from the native storyboard rule");
  const auto thumbnail = panels.at(0).toObject().value("thumbnail").toObject();
  require(thumbnail.value("exposed_drawing").toObject().value("level_id") ==
                  shown &&
              panels.at(2)
                  .toObject()
                  .value("thumbnail")
                  .toObject()
                  .value("exposed_drawing")
                  .isNull(),
          "Thumbnails do not name the topmost visible drawing");
  context.removeDocument(document.id);
}

void editsAndRejections(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QColor before = NavigationTags().m_lastTagColorUsed;
  result(dispatcher, "storyboard.panel.create",
         createParams(document, 8, "Opening"));
  require(NavigationTags().m_lastTagColorUsed == before,
          "Panel creation wrote the process-global tag color");
  require(rejected(dispatcher, document, "storyboard.panel.create",
                   createParams(document, 8, "Again")) == "already_exists",
          "A second panel at one frame was admitted");
  require(rejected(dispatcher, document, "storyboard.panel.create",
                   createParams(document, 9, "Two\nlines")) == "invalid_params",
          "A caption line break was admitted");
  require(
      rejected(dispatcher, document, "storyboard.panel.create",
               createParams(document, 9, "Tab", "a\tb")) == "invalid_params",
      "A notes tab was admitted");
  require(
      rejected(dispatcher, document, "storyboard.panel.update",
               addressed(document, {{"panel_frame", 8}})) == "invalid_params",
      "An empty update was admitted");
  require(rejected(dispatcher, document, "storyboard.panel.update",
                   addressed(document, {{"panel_frame", 3},
                                        {"caption", "x"}})) == "not_found",
          "An update of a missing panel was admitted");
  result(dispatcher, "storyboard.panel.create",
         createParams(document, 12, "Later"));
  require(rejected(dispatcher, document, "storyboard.panel.update",
                   addressed(document,
                             {{"panel_frame", 8}, {"new_panel_frame", 12}})) ==
              "already_exists",
          "A panel move onto another panel was admitted");
  const auto revision = document.revision;
  const auto same =
      result(dispatcher, "storyboard.panel.update",
             addressed(document, {{"panel_frame", 8}, {"caption", "Opening"}}));
  require(!same.value("changed").toBool() && document.revision == revision,
          "An identical update changed the revision");
  const auto noted = result(dispatcher, "storyboard.panel.update",
                            addressed(document, {{"panel_frame", 8},
                                                 {"new_panel_frame", 2},
                                                 {"notes", "Line\nbreak"}}));
  require(!noted.value("older_reader_compatible").toBool() &&
              noted.value("panel").toObject().value("panel_frame") == 2,
          "Notes did not mark the scene as needing a newer reader");
  result(dispatcher, "storyboard.panel.delete",
         addressed(document, {{"panel_frame", 2}}));
  require(rejected(dispatcher, document, "storyboard.panel.delete",
                   addressed(document, {{"panel_frame", 2}})) == "not_found",
          "A deleted panel was deleted twice");
  context.removeDocument(document.id);
}

void capacityAndPackages(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QString notes = QString::fromUtf8(
      "\"Quoted\" \\ caf\xc3\xa9 \xf0\x9f\x8e\xac\n</tag> & #x;");
  result(dispatcher, "storyboard.panel.create",
         createParams(document, 0, QString::fromUtf8("\xe6\x97\xa5 1"), notes));
  const QString directory =
      QDir(context.workspace()).filePath("panels-package");
  result(dispatcher, "scene.save_copy",
         addressed(document, {{"directory", directory}}));
  const auto opened = result(dispatcher, "scene.open", {{"path", directory}});
  auto& reopened    = context.document(opened.value("document_id").toString());
  const auto original =
      document.scene->getTopXsheet()->getNavigationTags()->m_tags;
  const auto restored =
      reopened.scene->getTopXsheet()->getNavigationTags()->m_tags;
  require(restored.size() == 1 && restored[0].m_notes == notes &&
              restored[0].m_label == original[0].m_label &&
              restored[0].m_color == original[0].m_color,
          "Panel text or color changed through a package");
  require(automationTagsDigest(*reopened.scene) ==
              automationTagsDigest(*document.scene),
          "Reopened panels have a different digest");
  auto* tags = document.scene->getTopXsheet()->getNavigationTags();
  for (int frame = 1; frame < 4096; ++frame)
    tags->m_tags.push_back(NavigationTags::Tag(frame, "", QColor(0, 0, 0)));
  require(rejected(dispatcher, document, "storyboard.panel.create",
                   createParams(document, 5000, "Over")) == "resource_limit",
          "More than 4096 panels were admitted");
  context.removeDocument(reopened.id);
  context.removeDocument(document.id);
}

}  // namespace

void runAutomationStoryboardTests(Context& context) {
  spansAndThumbnails(context);
  editsAndRejections(context);
  capacityAndPackages(context);
  std::cout << "automation-storyboard-test: spans, edits, limits and packages "
               "PASS\n";
}

}  // namespace GroundzeroAutomation
