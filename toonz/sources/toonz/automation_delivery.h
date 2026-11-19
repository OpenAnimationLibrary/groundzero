#pragma once

#include "automation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>
#include <vector>

namespace GroundzeroAutomation {

class Job;

// Delivery of render output into an output root (contract
// w3-publication-output slice P1 with amendment-w3-publication-1): a
// directory holding one folder per render job and delivery.json, published
// through the output-root publication engine.

// One verified file of a completed render job.
struct RenderedFile final {
  QString path;  // the private file, absolute
  QString name;  // its name inside the job's folder: frame.0001.png, movie.mp4
  QString role;  // frame or movie
  int frame    = -1;  // the scene frame of a frame file; -1 for a movie
  qint64 bytes = 0;
  QString sha256;  // as the render verified it
  QString mediaType;
};

// What a completed render job publishes: its record, the verified movie
// metadata (empty for a sequence) and its files in frame order.
struct RenderedOutput final {
  QJsonObject record;
  QJsonObject movie;
  std::vector<RenderedFile> files;
};

// The output of a render job of this process (automation_render.cpp); empty
// when `job` is not a render job.
//
// Throws:
//   Error: artifacts_unavailable when the job has not completed.
std::optional<RenderedOutput> renderedOutput(const Job& job);

QJsonArray deliveryOperations();
QJsonValue executeDelivery(Context& context, const QString& method,
                           const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationDeliveryTests();
#endif

}  // namespace GroundzeroAutomation
