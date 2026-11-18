#include "automation_storyboard.h"
#include "automation.h"

#include "toonz/navigationtags.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include <algorithm>
#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxPanels         = 4096;
constexpr int MaxFrame          = 1000000;
constexpr int MaxCaption        = 256;
constexpr int MaxNotes          = 4096;
constexpr qint64 MaxResultBytes = 1024 * 1024;

using Tag  = NavigationTags::Tag;
using Tags = std::vector<Tag>;

[[noreturn]] void invalid(const QString& message, QJsonObject details = {}) {
  throw Error("invalid_params", message, details);
}

NavigationTags* tagsOf(ToonzScene& scene) {
  return scene.getTopXsheet()->getNavigationTags();
}

QString textProblem(const QString& text, bool notes, int maximum) {
  const QVector<uint> points = text.toUcs4();
  if (points.size() > maximum) return "is too long";
  for (uint point : points)
    if (point < 0x20 && !(notes && point == '\n'))
      return notes ? "contains a control character other than LF"
                   : "contains a control character";
  return QString();
}

void requireText(const QString& text, const QString& field, bool notes) {
  const QString problem =
      textProblem(text, notes, notes ? MaxNotes : MaxCaption);
  if (!problem.isEmpty())
    invalid(field + " " + problem + ".", {{"field", field}});
}

bool hasNotes(const Tags& tags) {
  return std::any_of(tags.begin(), tags.end(),
                     [](const Tag& tag) { return !tag.m_notes.isEmpty(); });
}

int tagIndex(const Tags& tags, int frame) {
  for (int index = 0; index < int(tags.size()); ++index)
    if (tags[index].m_frame == frame) return index;
  return -1;
}

QJsonObject colorJson(const QColor& color) {
  return {{"r", color.red()}, {"g", color.green()}, {"b", color.blue()}};
}

QColor readColor(const QJsonObject& value) {
  return QColor(value.value("r").toInt(), value.value("g").toInt(),
                value.value("b").toInt());
}

// The drawing a storyboard panel shows: the topmost camstand-visible level
// column with a drawing at the panel frame, as StoryboardPane::buildPanels.
QJsonValue exposedDrawing(Document& document, int frame) {
  TXsheet* sheet = document.scene->getTopXsheet();
  for (int index = sheet->getColumnCount() - 1; index >= 0; --index) {
    TXshColumn* column = sheet->getColumn(index);
    if (!column || !column->isCamstandVisible()) continue;
    TXshLevelColumn* levels = column->getLevelColumn();
    if (!levels) continue;
    const TXshCell& cell = levels->getCell(frame);
    if (cell.isEmpty() || !cell.getSimpleLevel()) continue;
    return QJsonObject{
        {"level_id", document.idForLevel(cell.m_level.getPointer())},
        {"drawing_id", QJsonObject{{"number", cell.m_frameId.getNumber()},
                                   {"suffix", cell.m_frameId.getLetter()}}}};
  }
  return QJsonValue();
}

QJsonObject panelJson(Document& document, const Tags& tags, int index) {
  const Tag& tag   = tags[index];
  const int frames = document.scene->getTopXsheet()->getFrameCount();
  const int end    = index + 1 < int(tags.size())
                         ? tags[index + 1].m_frame - 1
                         : std::max(frames - 1, tag.m_frame);
  return {
      {"caption", tag.m_label},
      {"color", colorJson(tag.m_color)},
      {"end_frame", end},
      {"frame_count", end - tag.m_frame + 1},
      {"notes", tag.m_notes},
      {"panel_frame", tag.m_frame},
      {"thumbnail",
       QJsonObject{{"exposed_drawing", exposedDrawing(document, tag.m_frame)},
                   {"frame", tag.m_frame},
                   {"render_with", "render.preview"},
                   {"source_revision", document.revisionText()}}}};
}

Document& currentDocument(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (document.revisionText() != requiredString(params, "expected_revision"))
    throw Error(
        "revision_conflict",
        "The document changed; inspect its current revision first.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  validateAutomationStoryboard(*document.scene);
  return document;
}

QJsonObject panelResult(Document& document, int frame, bool changed) {
  const Tags& tags = tagsOf(*document.scene)->m_tags;
  return {{"changed", changed},
          {"older_reader_compatible", !hasNotes(tags)},
          {"panel", panelJson(document, tags, tagIndex(tags, frame))}};
}

void sortTags(NavigationTags* tags) {
  std::sort(tags->m_tags.begin(), tags->m_tags.end());
}

QJsonValue listPanels(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  const Tags& tags   = tagsOf(*document.scene)->m_tags;
  QJsonArray panels;
  for (int index = 0; index < int(tags.size()); ++index)
    panels.append(panelJson(document, tags, index));
  const QJsonObject result{
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"frame_count", document.scene->getTopXsheet()->getFrameCount()},
      {"notes_present", hasNotes(tags)},
      {"older_reader_compatible", !hasNotes(tags)},
      {"panels", panels}};
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
      MaxResultBytes)
    throw Error("resource_limit", "The panel list exceeds 1 MiB.");
  return result;
}

QJsonValue createPanel(Context& context, const QJsonObject& params) {
  Document& document    = currentDocument(context, params);
  NavigationTags* tags  = tagsOf(*document.scene);
  const int frame       = params.value("panel_frame").toInt();
  const QString caption = params.value("caption").toString();
  const QString notes   = params.value("notes").toString();
  requireText(caption, "caption", false);
  requireText(notes, "notes", true);
  if (tagIndex(tags->m_tags, frame) >= 0)
    throw Error("already_exists", "A panel already starts at that frame.",
                {{"panel_frame", frame}});
  if (int(tags->m_tags.size()) >= MaxPanels)
    throw Error("resource_limit", "A document admits at most 4096 panels.");
  Tag tag(frame, caption, readColor(params.value("color").toObject()));
  tag.m_notes = notes;
  context.markMutationStarted();
  tags->m_tags.push_back(tag);
  sortTags(tags);
  return panelResult(document, frame, true);
}

Tag updatedTag(const Tags& tags, const Tag& current,
               const QJsonObject& params) {
  Tag tag = current;
  if (params.contains("new_panel_frame")) {
    tag.m_frame = params.value("new_panel_frame").toInt();
    if (tag.m_frame != current.m_frame && tagIndex(tags, tag.m_frame) >= 0)
      throw Error("already_exists", "A panel already starts at that frame.",
                  {{"panel_frame", tag.m_frame}});
  }
  if (params.contains("caption")) {
    tag.m_label = params.value("caption").toString();
    requireText(tag.m_label, "caption", false);
  }
  if (params.contains("notes")) {
    tag.m_notes = params.value("notes").toString();
    requireText(tag.m_notes, "notes", true);
  }
  if (params.contains("color"))
    tag.m_color = readColor(params.value("color").toObject());
  return tag;
}

bool sameTag(const Tag& left, const Tag& right) {
  return left.m_frame == right.m_frame && left.m_label == right.m_label &&
         left.m_notes == right.m_notes && left.m_color == right.m_color;
}

QJsonValue updatePanel(Context& context, const QJsonObject& params) {
  Document& document   = currentDocument(context, params);
  NavigationTags* tags = tagsOf(*document.scene);
  const int frame      = params.value("panel_frame").toInt();
  const int index      = tagIndex(tags->m_tags, frame);
  if (index < 0)
    throw Error("not_found", "No panel starts at that frame.",
                {{"panel_frame", frame}});
  if (!params.contains("new_panel_frame") && !params.contains("caption") &&
      !params.contains("notes") && !params.contains("color"))
    invalid("Give at least one of new_panel_frame, caption, notes or color.");
  const Tag tag = updatedTag(tags->m_tags, tags->m_tags[index], params);
  if (sameTag(tag, tags->m_tags[index]))
    return panelResult(document, frame, false);
  context.markMutationStarted();
  tags->m_tags[index] = tag;
  sortTags(tags);
  return panelResult(document, tag.m_frame, true);
}

QJsonValue deletePanel(Context& context, const QJsonObject& params) {
  Document& document   = currentDocument(context, params);
  NavigationTags* tags = tagsOf(*document.scene);
  const int frame      = params.value("panel_frame").toInt();
  const int index      = tagIndex(tags->m_tags, frame);
  if (index < 0)
    throw Error("not_found", "No panel starts at that frame.",
                {{"panel_frame", frame}});
  context.markMutationStarted();
  tags->m_tags.erase(tags->m_tags.begin() + index);
  return QJsonObject{
      {"changed", true}, {"deleted", true}, {"panel_frame", frame}};
}

// ---------------------------------------------------------------------------
// Catalog.

QJsonObject textSchema(int maximum) {
  return {{"type", "string"}, {"minLength", 0}, {"maxLength", maximum}};
}

QJsonObject handleSchema(int maximum) {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strictObject(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject colorSchema() {
  return strictObject({{"r", integerSchema(0, 255)},
                       {"g", integerSchema(0, 255)},
                       {"b", integerSchema(0, 255)}},
                      {"r", "g", "b"});
}

QJsonObject tool(const QString& name, const QString& description,
                 QJsonObject fields, QJsonArray required, bool writes) {
  fields.insert("document_id", handleSchema(128));
  QJsonObject revision = handleSchema(20);
  revision.insert("pattern", "^(0|[1-9][0-9]*)$");
  fields.insert("expected_revision", revision);
  required.append("document_id");
  required.append("expected_revision");
  if (writes) {
    fields.insert("operation_id", handleSchema(128));
    required.append("operation_id");
  }
  QJsonObject descriptor = operation(name, description, {}, {}, writes);
  descriptor.insert("inputSchema", strictObject(fields, required));
  return descriptor;
}

}  // namespace

void validateAutomationStoryboard(ToonzScene& scene) {
  const Tags& tags = tagsOf(scene)->m_tags;
  if (int(tags.size()) > MaxPanels)
    throw Error("scene_limit", "The scene exceeds 4096 storyboard panels.");
  int previous = -1;
  for (const Tag& tag : tags) {
    if (tag.m_frame <= previous || tag.m_frame > MaxFrame ||
        !tag.m_color.isValid() ||
        !textProblem(tag.m_label, false, MaxCaption).isEmpty() ||
        !textProblem(tag.m_notes, true, MaxNotes).isEmpty())
      invalid("A storyboard panel is outside the detached panel profile.",
              {{"panel_frame", tag.m_frame}});
    previous = tag.m_frame;
  }
}

QString automationTagsDigest(ToonzScene& scene) {
  const Tags& tags = tagsOf(scene)->m_tags;
  if (tags.empty()) return QString();
  QJsonArray values;
  for (const Tag& tag : tags)
    values.append(QJsonArray{tag.m_frame, tag.m_label, tag.m_color.red(),
                             tag.m_color.green(), tag.m_color.blue(),
                             tag.m_notes});
  return QString::fromLatin1(
      QCryptographicHash::hash(
          QJsonDocument(values).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256)
          .toHex());
}

QJsonArray storyboardOperations() {
  const QJsonObject frame = integerSchema(0, MaxFrame);
  const QJsonObject panel{{"panel_frame", frame},
                          {"caption", textSchema(MaxCaption)},
                          {"notes", textSchema(MaxNotes)},
                          {"color", colorSchema()}};
  QJsonObject update = panel;
  update.insert("new_panel_frame", frame);
  return {
      tool("storyboard.panels",
           "List storyboard panels: spans, captions, notes, colors and the "
           "drawing each panel shows.",
           {}, {}, false),
      tool("storyboard.panel.create",
           "Start a panel at a frame with a caption, notes and color.", panel,
           {"panel_frame", "caption", "notes", "color"}, true),
      tool("storyboard.panel.update",
           "Change a panel's caption, notes or color, or move where it "
           "starts; exposures do not move.",
           update, {"panel_frame"}, true),
      tool("storyboard.panel.delete",
           "Remove a panel boundary; cells, sound and curves are unchanged.",
           {{"panel_frame", frame}}, {"panel_frame"}, true)};
}

QJsonValue executeStoryboard(Context& context, const QString& method,
                             const QJsonObject& params) {
  static const std::map<QString, QJsonValue (*)(Context&, const QJsonObject&)>
      handlers{{"storyboard.panels", listPanels},
               {"storyboard.panel.create", createPanel},
               {"storyboard.panel.update", updatePanel},
               {"storyboard.panel.delete", deletePanel}};
  const auto found = handlers.find(method);
  if (found == handlers.end())
    throw Error("unknown_method", "Unknown detached storyboard operation.");
  return found->second(context, params);
}

}  // namespace GroundzeroAutomation
