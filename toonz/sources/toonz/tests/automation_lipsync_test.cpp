#include "automation.h"
#include "automation_dispatch.h"
#include "automation_lipsync.h"

#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>

#include <iostream>
#include <stdexcept>
#include <vector>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QJsonObject request(Dispatcher& dispatcher, const QString& method,
                    QJsonObject params) {
  static int sequence = 0;
  if (method != "lipsync.inspect" && method != "column.cells")
    params.insert("operation_id",
                  "lipsync-test-" + QString::number(++sequence));
  return dispatcher.dispatch(
      {{"id", "lipsync"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const auto response = request(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}

QString code(const QJsonObject& response) {
  return response.value("error").toObject().value("code").toString();
}

QJsonObject addressed(Document& document, QJsonObject params = {}) {
  params.insert("document_id", document.id);
  params.insert("expected_revision", document.revisionText());
  return params;
}

QString writeFile(Context& context, const QString& name,
                  const QByteArray& bytes) {
  const QString path = QDir(context.workspace()).filePath(name);
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "Cannot write a lip-sync fixture");
  return path;
}

QJsonObject importParams(const QString& path, const QString& format,
                         const QString& set, double fps = 0) {
  QJsonObject params{{"path", path}, {"format", format}, {"shape_set", set}};
  if (fps > 0) params.insert("frame_rate", fps);
  return params;
}

QString importCode(Context& context, Dispatcher& dispatcher,
                   const QByteArray& bytes, const QString& format,
                   const QString& set, double fps = 0) {
  static int file = 0;
  const QString path =
      writeFile(context, "timing-" + QString::number(++file), bytes);
  return code(request(dispatcher, "lipsync.import",
                      importParams(path, format, set, fps)));
}

void strictParsing(Context& context) {
  Dispatcher dispatcher(context);
  const struct {
    QByteArray bytes;
    QString format, set;
    double fps;
    QString expected;
  } cases[] = {
      {"1 rest\n", "papagayo_dat", "preston_blair", 24, "invalid_asset"},
      {"MohoSwitch1\n1 rest\n1 ai\n", "papagayo_dat", "preston_blair", 24,
       "invalid_asset"},
      {"MohoSwitch1\n0 rest\n", "papagayo_dat", "preston_blair", 24,
       "invalid_asset"},
      {"MohoSwitch1\n1 rest\nnoise\n", "papagayo_dat", "preston_blair", 24,
       "invalid_asset"},
      {"MohoSwitch1\n1 smile\n", "papagayo_dat", "preston_blair", 24,
       "invalid_asset"},
      {"MohoSwitch1\n1 rest\n", "papagayo_dat", "preston_blair", 0,
       "invalid_params"},
      {"0.00\tA\n", "rhubarb_tsv", "rhubarb_basic", 24, "invalid_params"},
      {"0.00\tA\n", "rhubarb_tsv", "preston_blair", 0, "invalid_params"},
      {"0.1234567\tA\n", "rhubarb_tsv", "rhubarb_basic", 0, "invalid_asset"},
      {"0.50\tA\n0.25\tB\n", "rhubarb_tsv", "rhubarb_basic", 0,
       "invalid_asset"},
      {"0.00\tX\n", "rhubarb_tsv", "rhubarb_basic", 0, "invalid_asset"},
      {"0.00 A\n", "rhubarb_tsv", "rhubarb_basic", 0, "invalid_asset"},
      {"\xff\xfe", "rhubarb_tsv", "rhubarb_basic", 0, "invalid_asset"},
      {QByteArray("0.00\tA\n\0", 8), "rhubarb_tsv", "rhubarb_basic", 0,
       "invalid_asset"},
      {"{\"metadata\":{},\"mouthCues\":[{\"start\":0,\"end\":0.1,\"value\":"
       "\"A\"},{\"start\":0.2,\"end\":0.3,\"value\":\"B\"}]}",
       "rhubarb_json", "rhubarb_basic", 0, "invalid_asset"},
      {"{\"metadata\":{},\"mouthCues\":[{\"start\":0,\"end\":0.1,\"value\":"
       "\"A\",\"extra\":1}]}",
       "rhubarb_json", "rhubarb_basic", 0, "invalid_asset"},
      {"{\"metadata\":{},\"mouthCues\":[{\"start\":0.2,\"end\":0.2,"
       "\"value\":\"A\"}]}",
       "rhubarb_json", "rhubarb_basic", 0, "invalid_asset"},
      {"[]", "rhubarb_json", "rhubarb_basic", 0, "invalid_asset"},
      {QByteArray(1024 * 1024 + 1, 'A'), "rhubarb_tsv", "rhubarb_basic", 0,
       "resource_limit"},
  };
  for (const auto& item : cases)
    require(importCode(context, dispatcher, item.bytes, item.format, item.set,
                       item.fps) == item.expected,
            "A malformed timing file was not refused as expected");
}

struct Scene final {
  Document* document;
  QString level;
  QString column;
};

Scene drawingScene(Context& context, Dispatcher& dispatcher, double fps) {
  const auto created = result(dispatcher, "scene.create",
                              {{"name", "Lipsync"},
                               {"width_px", 32},
                               {"height_px", 32},
                               {"frame_rate", fps}});
  Document& document =
      context.document(created.value("document_id").toString());
  const QString level =
      result(dispatcher, "level.create",
             addressed(document, {{"name", "Mouth"}, {"format", "vector"}}))
          .value("level_id")
          .toString();
  for (int number = 1; number <= 4; ++number)
    result(dispatcher, "drawing.create",
           addressed(document, {{"level_id", level},
                                {"drawing_id", QJsonObject{{"number", number},
                                                           {"suffix", ""}}},
                                {"format", "vector"}}));
  const QString column = result(dispatcher, "column.create",
                                addressed(document, {{"name", "Lips"}}))
                             .value("column_id")
                             .toString();
  return {&document, level, column};
}

QJsonObject drawing(int number) { return {{"number", number}, {"suffix", ""}}; }

QJsonObject applyParams(const Scene& scene, const QString& analysis, double fps,
                        int start, const QJsonArray& mapping) {
  return addressed(*scene.document,
                   {{"analysis_id", analysis},
                    {"column_id", scene.column},
                    {"level_id", scene.level},
                    {"start_frame", start},
                    {"expected_frame_rate", fps},
                    {"mapping", mapping},
                    {"unmapped_policy", QJsonObject{{"kind", "reject"}}},
                    {"end_policy", QJsonObject{{"kind", "last_cue_end"}}},
                    {"overwrite", "replace_range"}});
}

std::vector<int> cellNumbers(Scene& scene, int first, int count) {
  std::vector<int> numbers;
  TXsheet* sheet = scene.document->scene->getXsheet();
  for (int row = first; row < first + count; ++row) {
    const TXshCell& cell = sheet->getCell(row, 0);
    numbers.push_back(cell.isEmpty() ? 0 : cell.m_frameId.getNumber());
  }
  return numbers;
}

// At 23.976 fps a cue at t microseconds starts at floor(t * 23976 / 1e9).
void rationalTimingAndDrops(Context& context) {
  Dispatcher dispatcher(context);
  Scene scene        = drawingScene(context, dispatcher, 23.976);
  const QString path = writeFile(
      context, "rational.tsv", "0.00\tA\n0.041709\tB\n0.041750\tC\n0.10\tA\n");
  const auto imported =
      result(dispatcher, "lipsync.import",
             importParams(path, "rhubarb_tsv", "rhubarb_basic"));
  const QJsonArray mapping{
      QJsonObject{{"shape", "A"}, {"drawing_id", drawing(1)}},
      QJsonObject{{"shape", "B"}, {"drawing_id", drawing(2)}},
      QJsonObject{{"shape", "C"}, {"drawing_id", drawing(3)}}};
  const auto applied =
      result(dispatcher, "lipsync.apply",
             applyParams(scene, imported.value("analysis_id").toString(),
                         23.976, 5, mapping));
  require(applied.value("dropped_cue_count").toInt() == 1 &&
              applied.value("dropped_cues")
                      .toArray()
                      .at(0)
                      .toObject()
                      .value("shape") == "B",
          "A cue inside one frame was not dropped");
  require(cellNumbers(scene, 4, 5) == std::vector<int>{0, 1, 3, 1, 0},
          "Microsecond cues landed on the wrong frames");
  const QString other = writeFile(context, "rest.dat", "MohoSwitch1\n2 rest\n");
  const auto dat =
      result(dispatcher, "lipsync.import",
             importParams(other, "papagayo_dat", "preston_blair", 24));
  require(code(request(
              dispatcher, "lipsync.apply",
              applyParams(scene, dat.value("analysis_id").toString(), 23.976, 0,
                          {QJsonObject{{"shape", "rest"},
                                       {"drawing_id", drawing(4)}}}))) ==
              "timing_conflict",
          "A 24 fps DAT applied to a 23.976 fps scene");
  context.removeDocument(scene.document->id);
}

void applyRejections(Context& context) {
  Dispatcher dispatcher(context);
  Scene scene = drawingScene(context, dispatcher, 24);
  const QString path =
      writeFile(context, "reject.dat", "MohoSwitch1\n1 ai\n3 other\n");
  const QString analysis =
      result(dispatcher, "lipsync.import",
             importParams(path, "papagayo_dat", "preston_blair", 24))
          .value("analysis_id")
          .toString();
  const QJsonArray partial{
      QJsonObject{{"shape", "ai"}, {"drawing_id", drawing(1)}}};
  const auto revision = scene.document->revision;
  const auto unmapped = request(dispatcher, "lipsync.apply",
                                applyParams(scene, analysis, 24, 0, partial));
  require(code(unmapped) == "invalid_params" &&
              unmapped.value("error")
                      .toObject()
                      .value("details")
                      .toObject()
                      .value("unmapped_shapes")
                      .toArray() == QJsonArray{"etc"},
          "An unmapped shape was not reported");
  const QJsonArray missing{
      QJsonObject{{"shape", "ai"}, {"drawing_id", drawing(9)}},
      QJsonObject{{"shape", "etc"}, {"drawing_id", drawing(2)}}};
  require(code(request(dispatcher, "lipsync.apply",
                       applyParams(scene, analysis, 24, 0, missing))) ==
              "missing_drawing",
          "A missing drawing was applied");
  require(code(request(dispatcher, "lipsync.apply",
                       applyParams(scene, analysis, 25, 0, partial))) ==
              "timing_conflict",
          "A wrong expected_frame_rate was applied");
  require(scene.document->revision == revision,
          "Rejected applies changed the revision");
  auto params = applyParams(scene, analysis, 24, 0, partial);
  params.insert("unmapped_policy", QJsonObject{{"kind", "use_drawing"},
                                               {"drawing_id", drawing(4)}});
  params.insert("end_policy",
                QJsonObject{{"kind", "hold_until_frame"}, {"frame", 6}});
  const auto applied = result(dispatcher, "lipsync.apply", params);
  require(
      cellNumbers(scene, 0, 8) == std::vector<int>{1, 1, 4, 4, 4, 4, 4, 0} &&
          applied.value("runs_count").toInt() == 2,
      "The fallback drawing or hold policy was not applied");
  context.removeDocument(scene.document->id);
}

}  // namespace

void runAutomationLipsyncTests(Context& context) {
  strictParsing(context);
  rationalTimingAndDrops(context);
  applyRejections(context);
  std::cout << "automation-lipsync-test: parsing, rational timing and apply "
               "PASS\n";
}

}  // namespace GroundzeroAutomation
